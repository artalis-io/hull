--
-- hull.compute_build — Shared source→WASM compilation helpers
--
-- Internal helper module used by:
--   stdlib/lua/hull/compute.lua  (hull compute build [name])
--   stdlib/lua/hull/build.lua    (auto-rebuild during hull build)
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
    local out = tool.spawn_read({"clang", "--version"})
    if out then return "clang" end

    return nil
end

-- ── Module discovery ──────────────────────────────────────────────────

--- Discover all WASM compute modules with source under `<app_dir>/compute/`.
--
-- A module is `<app_dir>/compute/<name>/<name>.c`. The convention is
-- documented in CLAUDE.md and `docs/wamr_architecture.md`.
--
-- @tparam string app_dir   Application directory (`.` for cwd).
-- @treturn {[i]: {name=string, src=string, wasm=string}, ...}  Sorted by name.
function M.discover_modules(app_dir)
    local compute_dir = (app_dir or ".") .. "/compute"
    local modules = {}
    if not tool.file_exists(compute_dir) then return modules end

    local files = tool.find_files(compute_dir, "*.c")
    local seen = {}
    for _, path in ipairs(files) do
        -- path is like compute/<name>/<name>.c
        local rel = path:sub(#(app_dir or ".") + 2)
        local name = rel:match("^compute/([^/]+)/([^/]+)%.c$")
        if name and not seen[name] then
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
    return modules
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

    local ok = tool.spawn({
        cc,
        "--target=wasm32-unknown-unknown",
        "-nostdlib",
        "-O2",
        "-flto",
        "-I", src_dir,
        "-Wl,--no-entry",
        "-Wl,--export=hull_process",
        "-Wl,--export=hull_version",
        "-Wl,--export=memory",
        "-Wl,--allow-undefined",
        "-Wl,--initial-memory=131072",
        "-Wl,--max-memory=67108864",
        "-o", module.wasm,
        module.src,
    })

    if not ok then
        return false, "clang failed compiling " .. module.src
    end
    return true, nil
end

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
    local modules = M.discover_modules(app_dir)
    local result = {
        modules  = modules,
        built    = {},
        skipped  = {},
        errors   = {},
        no_clang = false,
    }
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
