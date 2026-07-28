--
-- hull.analyze — Static import/require analysis for module declarations.
--
-- Usage: hull modules analyze [app_dir]
--        hull modules analyze --json [app_dir]
--
-- Scans every .lua and .js file under app_dir for require("hull.X") and
-- import "hull:X" patterns (skipping comments and string-literal text),
-- collects the set of canonical module names actually used, then compares
-- against the manifest's `modules` declaration. Reports:
--
--   - undeclared:  modules an app source imports but the manifest doesn't list
--   - unused:      modules the manifest declares but no source imports
--
-- Caveats:
--   - Indirect calls like `require("hull." .. var)` are not detected.
--     Apps that need them can declare statically.
--   - Names inside multi-line string literals delimited by `[[..]]`
--     (Lua) or `\`..\`` (JS template strings) are skipped correctly,
--     but pathological cases (e.g. nested `[==[..]==]`) are best-effort.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")

-- ── Argument parsing ────────────────────────────────────────────────

local function parse_args()
    local want_json = false
    local app_dir
    for i = 1, #arg do
        if arg[i] == "--json" then
            want_json = true
        elseif arg[i]:sub(1, 1) ~= "-" and not app_dir then
            app_dir = arg[i]
        end
    end
    return want_json, app_dir or "."
end

-- ── File enumeration ────────────────────────────────────────────────

local function find_source_files(app_dir)
    local lua_files = tool.find_files(app_dir, "*.lua") or {}
    local js_files  = tool.find_files(app_dir, "*.js")  or {}
    return lua_files, js_files
end

-- ── Lua scanner: strip comments + strings, find require() calls ─────
--
-- State machine walks one character at a time. Outside any string or
-- comment, it spots `require ( "hull.X" )` (any whitespace) and adds
-- "hull/X..." (with dots → slashes) to the result set.
--
-- Recognised hide states:
--   single-quote string  '..'    backslash escapes
--   double-quote string  ".."    backslash escapes
--   long bracket string  [[..]]  also [=[..]=] (any level)
--   line comment         -- ..   to end of line (but not --[[ ..)
--   block comment        --[[ .. ]]   also --[=[ .. ]=]

local function scan_lua(text)
    local out = {}
    local i = 1
    local n = #text
    while i <= n do
        local c = text:sub(i, i)

        -- line comment / block comment
        if c == '-' and text:sub(i, i + 1) == '--' then
            i = i + 2
            -- Check for long-bracket block comment opener
            local level = text:match("^(=*)%[", i)
            if level and text:sub(i + #level, i + #level) == '[' then
                local close = ']' .. level .. ']'
                local e = text:find(close, i + #level + 1, true)
                i = e and (e + #close) or (n + 1)
            else
                -- line comment
                local e = text:find("\n", i, true)
                i = e and (e + 1) or (n + 1)
            end

        -- long-bracket string literal
        elseif c == '[' then
            local level = text:match("^=*", i + 1) or ""
            if text:sub(i + 1 + #level, i + 1 + #level) == '[' then
                local close = ']' .. level .. ']'
                local e = text:find(close, i + 2 + #level, true)
                i = e and (e + #close) or (n + 1)
            else
                i = i + 1
            end

        -- single-/double-quoted string
        elseif c == '"' or c == "'" then
            local q = c
            i = i + 1
            while i <= n do
                local ch = text:sub(i, i)
                if ch == '\\' then i = i + 2
                elseif ch == q then i = i + 1 break
                else i = i + 1 end
            end

        else
            -- Look for `require ( "..." )` starting here.
            local s, e, quote, modname = text:find(
                "^require%s*%(%s*([\"'])hull%.([%w%._]+)%1%s*%)", i)
            if s then
                -- Normalize dots → slashes for canonical form.
                local canonical = "hull/" .. modname:gsub("%.", "/")
                out[canonical] = true
                i = e + 1
            else
                i = i + 1
            end
        end
    end
    return out
end

-- ── JS scanner ──────────────────────────────────────────────────────
--
-- Recognised hide states:
--   "..", '..', `..`  string/template-literal (template literals may
--                     contain ${...} interpolations; we still want to
--                     bail on the closing backtick, ignoring nested
--                     braces for this v1)
--   // .. to EOL      line comment
--   /* .. */          block comment

local function scan_js(text)
    local out = {}
    local i = 1
    local n = #text
    while i <= n do
        local c = text:sub(i, i)

        -- line comment
        if c == '/' and text:sub(i + 1, i + 1) == '/' then
            local e = text:find("\n", i, true)
            i = e and (e + 1) or (n + 1)

        -- block comment
        elseif c == '/' and text:sub(i + 1, i + 1) == '*' then
            local e = text:find("*/", i + 2, true)
            i = e and (e + 2) or (n + 1)

        -- string / template literal
        elseif c == '"' or c == "'" or c == '`' then
            local q = c
            i = i + 1
            while i <= n do
                local ch = text:sub(i, i)
                if ch == '\\' then i = i + 2
                elseif ch == q then i = i + 1 break
                else i = i + 1 end
            end

        else
            -- `import ... from "hull:X"` — capture only the module string.
            -- Handles `import x from`, `import {x} from`, `import * as x from`.
            local s, e, modname = text:find(
                "^import[%s%w_${},*]+from%s*[\"']hull:([%w_:]+)[\"']", i)
            if not s then
                -- Dynamic `import("hull:X")`
                s, e, modname = text:find(
                    "^import%s*%(%s*[\"']hull:([%w_:]+)[\"']%s*%)", i)
            end
            if s then
                local canonical = "hull/" .. modname:gsub(":", "/")
                out[canonical] = true
                i = e + 1
            else
                i = i + 1
            end
        end
    end
    return out
end

-- ── Manifest module collection ──────────────────────────────────────

-- Intrinsic modules are always admitted by the resolver (no declaration
-- needed) — seed them into the "declared" set so the analyzer doesn't
-- flag imports of `hull:app` / `hull:log` / `hull:json` as undeclared.
-- Mirrors hl_module_registry: every entry with intrinsic = 1.
local INTRINSICS = {
    ["hull/app"]  = true,
    ["hull/log"]  = true,
    ["hull/json"] = true,
}

local function collect_declared(manifest)
    -- Returns a set of canonical names ({ ["hull/crypto"]=true, ... }).
    -- Manifest format: an array of "vendor/name@version" specs.
    -- Legacy keyed form is also tolerated (values, not keys, hold the spec).
    local out = {}
    for k, _ in pairs(INTRINSICS) do out[k] = true end
    if not manifest or not manifest.modules then return out end
    local m = manifest.modules
    if #m > 0 then
        for _, spec in ipairs(m) do
            local at = spec:find("@", 1, true)
            local name = at and spec:sub(1, at - 1) or spec
            out[name] = true
        end
    else
        for _, spec in pairs(m) do
            local at = spec:find("@", 1, true)
            local name = at and spec:sub(1, at - 1) or spec
            out[name] = true
        end
    end
    return out
end

-- ── Comparison ──────────────────────────────────────────────────────

local function diff_sets(declared, used_by_file)
    -- used_by_file: array of { path, set } entries.
    -- Returns: undeclared = [{ path, name }, ...], unused = [name, ...]
    local undeclared = {}
    local used_anywhere = {}
    for _, e in ipairs(used_by_file) do
        for name, _ in pairs(e.set) do
            used_anywhere[name] = true
            if not declared[name] then
                undeclared[#undeclared + 1] = { path = e.path, module = name }
            end
        end
    end
    local unused = {}
    for name, _ in pairs(declared) do
        -- Don't surface intrinsics as "unused" — they're always declared
        -- automatically regardless of whether the app imports them.
        if not used_anywhere[name] and not INTRINSICS[name] then
            unused[#unused + 1] = name
        end
    end
    table.sort(unused)
    table.sort(undeclared, function(a, b)
        if a.path == b.path then return a.module < b.module end
        return a.path < b.path
    end)
    return undeclared, unused
end

-- ── Main ────────────────────────────────────────────────────────────

local function main()
    local want_json, app_dir = parse_args()

    local entry = app_dir .. "/app.lua"
    local runtime = "lua"
    if not tool.file_exists(entry) then
        local js_entry = app_dir .. "/app.js"
        if tool.file_exists(js_entry) then
            entry = js_entry
            runtime = "js"
        else
            tool.stderr("hull modules analyze: no app.lua or app.js in " ..
                        app_dir .. "\n")
            tool.exit(1)
        end
    end

    -- Load app to extract manifest. App load can fail in tool-mode for
    -- legitimate reasons (apps that require relative JSON files via
    -- `require("./data.json")` — the tool VM lacks the real
    -- app-context's filesystem-fallback wiring). Treat it as
    -- "skipping analysis" rather than a hard failure so `hull check`
    -- still runs the rest of its pipeline. A genuine import problem
    -- still surfaces at server startup via the resolver.
    local chunk, err = tool.loadfile(entry)
    if not chunk then
        tool.stderr("hull modules analyze: cannot load " .. entry ..
                    " (" .. tostring(err) .. ") — skipping\n")
        return
    end
    local ok, run_err = pcall(chunk)
    if not ok then
        tool.stderr("hull modules analyze: app load error (" ..
                    tostring(run_err) .. ") — skipping\n")
        return
    end
    local manifest = app.get_manifest()
    local declared = collect_declared(manifest)

    -- Scan only source files matching the entry-point runtime. Apps
    -- run as Lua OR JS; the two variants have independent manifests,
    -- so cross-runtime scanning produces false positives.
    local lua_files, js_files = find_source_files(app_dir)
    local used = {}
    if runtime == "lua" then
        for _, p in ipairs(lua_files) do
            local data = tool.read_file(p)
            if data then used[#used + 1] = { path = p, set = scan_lua(data) } end
        end
    else
        for _, p in ipairs(js_files) do
            local data = tool.read_file(p)
            if data then used[#used + 1] = { path = p, set = scan_js(data) } end
        end
    end

    local undeclared, unused = diff_sets(declared, used)

    if want_json then
        print(json.encode({
            app_dir    = app_dir,
            declared   = (function()
                local t = {}; for k, _ in pairs(declared) do t[#t+1] = k end
                table.sort(t); return t
            end)(),
            undeclared = undeclared,
            unused     = unused,
        }))
        if #undeclared > 0 then tool.exit(1) end
        return
    end

    -- Human-readable.
    if #undeclared == 0 and #unused == 0 then
        print("hull modules analyze: OK — every imported module is declared, " ..
              "no unused declarations.")
        return
    end

    if #undeclared > 0 then
        print("Undeclared imports (will fail at runtime):")
        for _, x in ipairs(undeclared) do
            print(string.format("  %s — %s", x.path, x.module))
        end
        print("")
        print("Fix: add the missing modules to app.manifest's `modules` array.")
        print("Example: modules = { \"" .. undeclared[1].module .. "@1\", ... }")
    end

    if #unused > 0 then
        if #undeclared > 0 then print("") end
        print("Declared but never imported (capability surface larger than needed):")
        for _, name in ipairs(unused) do
            print("  " .. name)
        end
    end

    -- Undeclared imports are blocking; unused is advisory.
    if #undeclared > 0 then tool.exit(1) end
end

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
