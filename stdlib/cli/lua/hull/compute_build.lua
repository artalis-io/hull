--
-- hull.compute_build - Shared source→WASM compilation helpers
--
-- Internal helper module used by:
--   stdlib/cli/lua/hull/compute.lua  (hull compute build [name])
--   stdlib/cli/lua/hull/build.lua    (auto-rebuild during hull build)
--
-- Centralizing this avoids drift between the dev-tool path and the
-- release-build path: same clang lookup, same flags, same module
-- layout discovery.
--
-- This module is loaded in the `hull tool` Lua harness, where `tool.*`
-- globals are provided. It is also safe to load from app code but apps
-- have no reason to.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- ── clang lookup ──────────────────────────────────────────────────────

--- Find a clang binary capable of targeting wasm32.
--
-- Prefers Homebrew LLVM (bundles wasm-ld for the wasm32 target).
-- Falls back to system clang (works if wasm-ld is in PATH).
--
-- @return string|nil  Path to clang, or nil if no suitable clang found.
function M.find_clang()
    local brew_paths = {
        "/opt/homebrew/opt/llvm@18/bin/clang",
        "/opt/homebrew/opt/llvm/bin/clang",
        "/usr/local/opt/llvm@18/bin/clang",
        "/usr/local/opt/llvm/bin/clang",
    }
    for _, p in ipairs(brew_paths) do
        if tool.file_exists(p) then return p end
    end

    -- Fall back to system clang (works if wasm-ld is in PATH).
    --
    -- Resolve the NAME to a path before spawning it. A bare "clang" relies on
    -- exec's own PATH search, which an APE cannot depend on for the PATH
    -- Windows hands it - that is exactly why hl_driver_resolve_name exists for
    -- cc / cosmocc / the linkers (src/hull/compiler.c: "exec's own PATH search
    -- cannot be relied on for this on Windows"). This lookup never got the same
    -- treatment, so `hull compute build` reported "clang not found" on a host
    -- with clang on PATH, and every wasm32 compute suite failed there for a
    -- reason that had nothing to do with the toolchain being absent.
    --
    -- tool.find_tool goes through hl_tools_lookup_path -> hl_host_find_in_path,
    -- which knows the PATH forms a Windows host uses and the .exe/.com suffixes.
    -- The spawn allowlist matches on the BASENAME, so an absolute path is still
    -- admitted as "clang".
    local resolved = tool.find_tool and tool.find_tool("clang")
    -- No bare-name fallback: a miss here is a miss on PATH too, and the spawn
    -- resolves a bare name through the same walker (audit 17).
    if resolved and tool.spawn_read({resolved, "--version"}) then
        return resolved
    end

    return nil
end

-- ── Module discovery ──────────────────────────────────────────────────

--- Discover all WASM compute modules with source under `<app_dir>/compute/`.
--
-- A module is `<app_dir>/compute/<name>/<name>.c`. The convention is
-- documented in CLAUDE.md and `docs/wamr_architecture.md`.
--
-- A module name is the rule `hull compute new` enforces: letters, digits,
-- `_` and `-`. A name discovered in a cloned app tree is held to it too: it
-- becomes part of the paths handed to clang and wamrc (audit 18).
function M.valid_module_name(name)
    return type(name) == "string" and name:match("^[A-Za-z0-9_%-]+$") ~= nil
end

function M.invalid_name_message(name)
    return "invalid compute module name '" .. tostring(name) ..
           "' (names may contain only letters, digits, underscores and hyphens)"
end

-- @tparam string app_dir   Application directory (`.` for cwd).
-- @treturn {[i]: {name=string, src=string, wasm=string}, ...}  Sorted by name.
-- @treturn {[i]: string}  Directory names under compute/ holding a module
--   source whose name is not valid (see valid_module_name). Sorted.
function M.discover_modules(app_dir)
    local compute_dir = (app_dir or ".") .. "/compute"
    local modules = {}
    local invalid = {}
    if not tool.file_exists(compute_dir) then return modules, invalid end
    -- An app's compute/ is its own: through a link, clang read sources from
    -- and wrote .wasm / hull_compute.h into wherever it pointed (audit 21).
    -- The commands refuse such a root with a message; nothing is found here.
    if tool.is_symlink(compute_dir) ~= false then return modules, invalid end

    local files = tool.find_files(compute_dir, "*.c")
    local seen = {}
    for _, path in ipairs(files) do
        -- path is like compute/<name>/<name>.c
        local rel = path:sub(#(app_dir or ".") + 2)
        local name = rel:match("^compute/([^/]+)/([^/]+)%.c$")
        if name and not seen[name] and not M.valid_module_name(name) then
            seen[name] = true
            invalid[#invalid + 1] = name
        elseif name and not seen[name] then
            seen[name] = true
            modules[#modules + 1] = {
                name = name,
                src  = path,
                wasm = (app_dir or ".") .. "/compute/" .. name .. ".wasm",
            }
        end
    end

    -- Stable ordering (helps reproducible builds + readable logs).
    table.sort(modules, function(a, b) return a.name < b.name end)
    table.sort(invalid)
    return modules, invalid
end

--- Is the WASM artifact stale relative to its source?
--
-- Stale if `.wasm` is missing or `<name>.c` has a newer mtime.
--
-- @tparam string src_path
-- @tparam string wasm_path
-- @treturn boolean
function M.is_stale(src_path, wasm_path)
    if not tool.file_exists(wasm_path) then return true end
    local src_mtime  = tool.file_mtime(src_path)
    local wasm_mtime = tool.file_mtime(wasm_path)
    if not src_mtime or not wasm_mtime then
        -- If mtime isn't available we conservatively rebuild.
        return true
    end
    return src_mtime > wasm_mtime
end

-- ── Compile one module ────────────────────────────────────────────────
--
-- A module's sources are the app's (a cloned repo's), compiled by hull build
-- on the developer's machine. `.incbin` (in an asm statement) and C23
-- `#embed` make the compiler read ANY file into the .wasm - ~/.ssh/id_rsa,
-- where nothing but a Landlock tool sandbox stops it - and app code can then
-- read it back at run time. A compute plugin has no use for either.
--
-- A lexical scan of the sources cannot see through the preprocessor (a
-- `#include "data.txt"`, the `%:embed` digraph, `".inc\x62in"`, stringizing;
-- audit 15), so the module is built in two steps and the compiler itself
-- reports what it read:
--
--   1. `clang -E -MD -MF deps.d -o pre.i`: the preprocessed text plus the
--      list of every file the preprocessor opened - the #included headers AND
--      every #embed'ed file. Each must resolve (realpath, so a symlink counts
--      where it points) inside compute/<name>/, where the freestanding
--      hull_compute.h lives. A dependency the preprocessed text never entered
--      (it has no `# N "file"` line marker) was embedded, not included, and is
--      refused even inside the module.
--   2. The preprocessed text is searched for `incbin` / `.include` (an asm
--      directive that reads a file at assembly time), with every string
--      literal's escapes decoded, for any inline assembly at all (`asm` /
--      `__asm` / `__asm__` outside literals: the assembler's macro language
--      can build `.incbin` at assembly time; audit 29), and for any
--      directive left in it (clang
--      still runs directives in `-x cpp-output` input, and a macro can make
--      one; audit 16), then compiled AS IS (`-x cpp-output`), so what was
--      checked is exactly what is built.
--   3. That compile writes its own dependency list; anything in it besides
--      pre.i means the check missed a directive, and the output is deleted.

local function is_windows()
    return tool.host_os and tool.host_os() == "windows"
end

-- A path as tool.realpath sees it: Windows clang writes backslashes.
local function host_path(p)
    if is_windows() then p = p:gsub("\\", "/") end
    return p
end

-- Canonical form for comparisons (Windows paths compare without case).
local function canon(p)
    local r = tool.realpath(host_path(p))
    if not r then return nil end
    if is_windows() then r = r:lower() end
    return (r:gsub("/+$", ""))
end

-- The prerequisites of a make rule written by `clang -MD`: one target, then
-- whitespace-separated paths, `\`-newline continuations, `\ ` / `\#` escapes
-- and `$$` for `$`. Any other backslash is part of the path (Windows).
local function parse_deps(text)
    local t = text:gsub("\\\r?\n", " ")
    local body = t:match("^.-:%s(.*)$")
    if not body then return nil end
    local deps, cur, i = {}, {}, 1
    while i <= #body do
        local c = body:sub(i, i)
        local n = body:sub(i + 1, i + 1)
        if c == "\\" and (n == " " or n == "#" or n == "\t") then
            cur[#cur + 1] = n; i = i + 2
        elseif c == "$" and n == "$" then
            cur[#cur + 1] = "$"; i = i + 2
        elseif c:match("%s") then
            if #cur > 0 then deps[#deps + 1] = table.concat(cur); cur = {} end
            i = i + 1
        else
            cur[#cur + 1] = c; i = i + 1
        end
    end
    if #cur > 0 then deps[#deps + 1] = table.concat(cur) end
    return deps
end

local SIMPLE_ESC = {
    a = "\a", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t", v = "\v",
    ["\\"] = "\\", ["'"] = "'", ['"'] = '"', ["?"] = "?",
}

-- Decode the body of a C string / character literal. A code point past
-- ASCII becomes "?" (no directive name has one).
local function decode_c_string(s)
    local out, i = {}, 1
    while i <= #s do
        local c = s:sub(i, i)
        if c ~= "\\" then
            out[#out + 1] = c; i = i + 1
        else
            local n = s:sub(i + 1, i + 1)
            if SIMPLE_ESC[n] then
                out[#out + 1] = SIMPLE_ESC[n]; i = i + 2
            elseif n:match("[0-7]") then
                local oct = s:match("^[0-7][0-7]?[0-7]?", i + 1)
                local v = tonumber(oct, 8)
                out[#out + 1] = v < 128 and string.char(v) or "?"
                i = i + 1 + #oct
            elseif n == "x" then
                local hex = s:match("^%x+", i + 2) or ""
                local v = tonumber(hex ~= "" and hex:sub(-8) or "0", 16)
                out[#out + 1] = v < 128 and string.char(v) or "?"
                i = i + 2 + #hex
            elseif n == "u" or n == "U" then
                local len = n == "u" and 4 or 8
                local hex = s:match("^" .. string.rep("%x", len), i + 2)
                local v = hex and tonumber(hex, 16) or 0x3f
                out[#out + 1] = v < 128 and string.char(v) or "?"
                i = i + 2 + (hex and len or 0)
            else
                out[#out + 1] = n; i = i + 2
            end
        end
    end
    return table.concat(out)
end

-- Every string and character literal of preprocessed C, decoded and joined
-- (adjacent literals concatenate, so ".inc" "bin" reads as ".incbin"), plus
-- the `"file"` of each `# N "file"` line marker. Comments are gone after -E.
local function scan_preprocessed(text)
    local lits, markers, code = {}, {}, {}
    local i, n = 1, #text
    local bol = true
    while i <= n do
        local c = text:sub(i, i)
        -- `# N "file" flags`: the file name runs to the line's last quote.
        local eol, f
        if bol and c == "#" then
            eol = text:find("\n", i, true) or (n + 1)
            f = text:sub(i, eol - 1):match('^#%s*%d+%s+"(.*)"')
        end
        if f then
            markers[#markers + 1] = decode_c_string(f)
            code[#code + 1] = " "
            i = eol
        elseif c == '"' or c == "'" then
            local j = i + 1
            while j <= n do
                local d = text:sub(j, j)
                if d == "\\" then j = j + 2
                elseif d == c or d == "\n" then break
                else j = j + 1 end
            end
            lits[#lits + 1] = decode_c_string(text:sub(i + 1, j - 1))
            code[#code + 1] = " "
            i = j + 1
            bol = false
        else
            code[#code + 1] = c
            bol = (c == "\n") or (bol and (c == " " or c == "\t"))
            i = i + 1
        end
    end
    return table.concat(lits), markers, table.concat(code)
end

-- The pragmas a compute module may keep in its preprocessed text: loop and
-- diagnostic control, floating-point and packing. Nothing here names a file
-- (unlike `GCC dependency`, or the MS `include_alias`).
local PRAGMA_OK = {
    unroll = true, nounroll = true, unroll_and_jam = true,
    nounroll_and_jam = true, pack = true, STDC = true,
}
local PRAGMA_OK_NS = {
    clang = { loop = true, diagnostic = true, fp = true },
    GCC   = { diagnostic = true },
}

-- Does the preprocessed text `pre` still hold a preprocessor directive?
-- clang runs directives in `-x cpp-output` input too, and a macro can make
-- one: `#define E # embed "/abs/secret"` expands, on a line of its own, to
-- ` # embed "/abs/secret"`, which -E leaves alone and the compile step then
-- obeys (audit 16). Only `# N "file" flags` line markers and the pragmas
-- above may remain. Returns a reason or nil.
local function preprocessed_directive(pre)
    -- Phase 2 first: a `\`-newline joins two lines into one.
    local text = pre:gsub("\\\r?\n", "")
    if text:find("_Pragma", 1, true) or text:find("__pragma", 1, true) then
        return "a _Pragma operator"
    end
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
        local l = line:gsub("^%s+", ""):gsub("%s+$", "")
        local rest
        if l:sub(1, 1) == "#" then rest = l:sub(2)
        elseif l:sub(1, 2) == "%:" then rest = l:sub(3)   -- the # digraph
        elseif l:sub(1, 3) == "??=" then rest = l:sub(4)  -- the # trigraph
        end
        if rest then
            rest = rest:gsub("^%s+", "")
            local marker = l:sub(1, 1) == "#" and (rest:match("^%d+$")
                or rest:match('^%d+%s+".*"[%d%s]*$'))
            if not marker then
                local ok
                if l:sub(1, 1) == "#" then
                    local w1, tail = rest:match("^pragma%s+([%a_][%w_]*)(.*)$")
                    if w1 and PRAGMA_OK[w1] then
                        ok = true
                    elseif w1 and PRAGMA_OK_NS[w1] then
                        local w2 = tail:match("^%s+([%a_][%w_]*)")
                        ok = w2 and PRAGMA_OK_NS[w1][w2] or false
                    end
                end
                if not ok then
                    return "a preprocessor directive made by a macro ("
                        .. l:sub(1, 60) .. ")"
                end
            end
        end
    end
    return nil
end

-- Does the preprocessed text `pre` ask the assembler to read a file?
-- Returns a reason or nil.
-- The keywords that open inline assembly (GNU / clang; `_asm` is the
-- -fms-extensions spelling).
local ASM_KEYWORDS = { asm = true, __asm = true, __asm__ = true, _asm = true }

local function preprocessed_reads_files(pre)
    -- Phase 2 first, as the compile step will: a `\`-newline (or its
    -- `??/` trigraph) joins the pieces of a split identifier.
    local text = pre:gsub("%?%?/\r?\n", ""):gsub("\\\r?\n", "")
    local lits, _, code = scan_preprocessed(text)
    local hay = (text .. "\n" .. lits):lower()
    if hay:find("incbin", 1, true) then return "an .incbin directive" end
    if lits:lower():find("%.include") then return "an asm .include directive" end
    -- Any inline assembly at all (audit 29): the assembler's own macro
    -- language builds a directive at assembly time (`.macro m a` ...
    -- `.inc\()\a` with `m bin` makes `.incbin`), so no scan of the text can
    -- rule a file read out. A freestanding hull_process module has no use
    -- for asm; outside string literals (markers dropped), it is refused.
    for word in code:gmatch("[%a_][%w_]*") do
        if ASM_KEYWORDS[word] then
            return "inline assembly (`" .. word .. "`)"
        end
    end
    return nil
end

--- Compile a single source file to a WASM module.
--
-- Invokes clang with the freestanding wasm32 flags that match the
-- `hull_compute.h` ABI. Caller is responsible for finding clang.
--
-- @tparam string cc        Path to clang.
-- @tparam table  module    { name, src, wasm } as returned by discover_modules().
-- @treturn boolean ok      True on success.
-- @treturn string  err     Error message on failure, nil on success.
function M.compile_module(cc, module)
    local src_dir = module.src:match("(.*)/[^/]+$") or "."
    local root = canon(src_dir)
    if not root then
        return false, "cannot resolve " .. src_dir
    end

    local tmp = tool.tmpdir()
    if not tmp then return false, "cannot create a temporary directory" end
    local deps_path = tmp .. "/deps.d"
    local pre_path  = tmp .. "/pre.i"
    local deps2_path = tmp .. "/deps2.d"
    local function cleanup()
        tool.remove_file(deps_path)
        tool.remove_file(deps2_path)
        tool.remove_file(pre_path)
        tool.rmdir(tmp)
    end
    local function refuse(msg)
        cleanup()
        return false, msg
    end

    local ok = tool.spawn({
        cc,
        "--target=wasm32-unknown-unknown",
        "-nostdlib",
        "-O2",
        "-I", src_dir,
        "-E", "-MD", "-MF", deps_path,
        "-o", pre_path,
        module.src,
    })
    if not ok then return refuse("clang failed preprocessing " .. module.src) end

    local deps = parse_deps(tool.read_file(deps_path) or "")
    local pre = tool.read_file(pre_path)
    if not deps or #deps == 0 or not pre then
        return refuse("clang wrote no dependency list for " .. module.src)
    end

    local _, markers = scan_preprocessed(pre)
    local entered = {}
    for _, f in ipairs(markers) do
        local c = canon(f)
        if c then entered[c] = true end
    end

    for _, d in ipairs(deps) do
        local c = canon(d)
        if not c or c:sub(1, #root + 1) ~= root .. "/" then
            return refuse(module.src .. " reads " .. d .. ", outside "
                .. src_dir .. "; a compute module may only include its own files")
        end
        if not entered[c] then
            return refuse(module.src .. " uses an #embed directive (" .. d
                .. "), which can pull a file into the module; compute sources may not")
        end
    end

    local what = preprocessed_directive(pre) or preprocessed_reads_files(pre)
    if what then
        return refuse(module.src .. " uses " .. what .. ", which can pull a file "
            .. "from outside the app into the module; compute sources may not")
    end

    -- The compile step reports every file IT opened too (the driver drops
    -- -MD for cpp-output input, so it goes to cc1 directly): anything but
    -- pre.i means a directive got through, and the output is thrown away.
    ok = tool.spawn({
        cc,
        "--target=wasm32-unknown-unknown",
        "-nostdlib",
        "-O2",
        "-flto",
        "-Wl,--no-entry",
        "-Wl,--export=hull_process",
        "-Wl,--export=hull_version",
        "-Wl,--export=memory",
        "-Wl,--allow-undefined",
        "-Wl,--initial-memory=131072",
        "-Wl,--max-memory=67108864",
        "-Xclang", "-dependency-file", "-Xclang", deps2_path,
        "-Xclang", "-MT", "-Xclang", "pre",
        "-o", module.wasm,
        "-x", "cpp-output", pre_path,
    })
    if not ok then
        tool.remove_file(module.wasm)
        return refuse("clang failed compiling " .. module.src)
    end

    local deps2 = parse_deps(tool.read_file(deps2_path) or "")
    local pre_c = canon(pre_path)
    local bad = (not deps2 or #deps2 == 0 or not pre_c) and "(no dependency list)"
    for _, d in ipairs(deps2 or {}) do
        if not bad and canon(d) ~= pre_c then bad = d end
    end
    if bad then
        tool.remove_file(module.wasm)
        return refuse(module.src .. ": the compile step read " .. bad
            .. " besides the checked preprocessed text; compute sources may not")
    end
    cleanup()
    return true, nil
end

-- Exposed for the stdlib self-test (stdlib/cli/lua/hull/tests).
M._parse_deps = parse_deps
M._decode_c_string = decode_c_string
M._preprocessed_reads_files = preprocessed_reads_files
M._preprocessed_directive = preprocessed_directive

-- ── High-level: build all stale modules ───────────────────────────────

--- Build every stale compute module under `<app_dir>/compute/`.
--
-- Discovers modules, filters to the stale ones, compiles each.
-- Returns counts so callers can report appropriately.
--
-- If `mode == "force"` every discovered module is rebuilt regardless
-- of mtime. Default mode `"stale"` only rebuilds when needed.
--
-- @tparam string app_dir   App directory.
-- @tparam[opt="stale"] string mode  "stale" or "force".
-- @treturn table  {
--                   modules = {discovered list},
--                   built   = {compiled names},
--                   skipped = {up-to-date names},
--                   errors  = {{name, err}, ...},
--                   no_clang = boolean,  -- true if clang missing AND there
--                                        -- was work to do
--                 }
function M.build_all(app_dir, mode)
    mode = mode or "stale"
    local modules, invalid = M.discover_modules(app_dir)
    local result = {
        modules  = modules,
        built    = {},
        skipped  = {},
        errors   = {},
        no_clang = false,
    }
    -- A source under a name the rule refuses is an error, never compiled.
    if #invalid > 0 then
        for _, name in ipairs(invalid) do
            result.errors[#result.errors + 1] = {
                name = name, err = M.invalid_name_message(name),
            }
        end
        return result
    end
    if #modules == 0 then return result end

    -- Determine the list to actually compile.
    local todo = {}
    for _, m in ipairs(modules) do
        if mode == "force" or M.is_stale(m.src, m.wasm) then
            todo[#todo + 1] = m
        else
            result.skipped[#result.skipped + 1] = m.name
        end
    end
    if #todo == 0 then return result end

    local cc = M.find_clang()
    if not cc then
        result.no_clang = true
        for _, m in ipairs(todo) do
            result.errors[#result.errors + 1] = {
                name = m.name,
                err  = "clang not found (install brew llvm@18 or apt install clang lld)",
            }
        end
        return result
    end

    for _, m in ipairs(todo) do
        local ok, err = M.compile_module(cc, m)
        if ok then
            result.built[#result.built + 1] = m.name
        else
            result.errors[#result.errors + 1] = { name = m.name, err = err }
        end
    end
    return result
end

return M
