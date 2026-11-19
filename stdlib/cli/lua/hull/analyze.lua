--
-- hull.analyze - Static import/require analysis for module declarations.
--
-- Usage: hull modules analyze [app_dir]
--        hull modules analyze --json [app_dir]
--
-- Scans the app's sources in its entry's language (.lua; or .js / .ts /
-- .mts) for require("hull.X") and import "hull:X" (skipping comments and
-- string-literal text), and for the `app` methods a module adds
-- (app.get / app.use / app.router: hull/http-server, app.every / app.daily:
-- hull/timers, app.sse: hull/web/sse, app.ws: hull/web/ws-server). Each
-- name is taken to its registered module (hull.crypto.sealbox to
-- hull/crypto, as the loader gates it), then compared with the manifest.
-- Reports:
--
--   - undeclared:  modules a source uses that the resolved manifest does
--                  not admit (the module resolver's set: the declared
--                  modules, their dependencies and the intrinsic hull/app),
--                  so the app fails to load or the call raises
--   - unused:      modules the manifest declares that no source uses
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
    -- JavaScript, and TypeScript: its imports are JavaScript's. A declaration
    -- file (.d.ts) is not a source; `hull types` installs dozens of them.
    local source_ext = require("hull.project.registry").source_ext
    local js_files = {}
    for _, pat in ipairs({ "*.js", "*.ts", "*.mts" }) do
        for _, p in ipairs(tool.find_files(app_dir, pat) or {}) do
            if source_ext(p) then js_files[#js_files + 1] = p end
        end
    end
    table.sort(js_files)
    return lua_files, js_files
end

-- ── app decorations ────────────────────────────────────────────────
--
-- The `app` methods a declared module adds (mod_app.c in each runtime): an
-- app that calls one uses that module, and without it the call raises
-- ("attempt to call a nil value" / "not a function").
local DECORATIONS = {
    get = "hull/http-server", post = "hull/http-server", put = "hull/http-server",
    delete = "hull/http-server", del = "hull/http-server", patch = "hull/http-server",
    options = "hull/http-server", use = "hull/http-server", use_post = "hull/http-server",
    usePost = "hull/http-server", router = "hull/http-server",
    every = "hull/timers", daily = "hull/timers",
    sse = "hull/web/sse",
    ws = "hull/web/ws-server",
    transform = "hull/build-plugin",
}

-- Whether the byte before position i continues an identifier (so a match
-- at i is inside a longer name: `myapp.get`, `x.require`).
local function ident_before(text, i)
    return i > 1 and text:sub(i - 1, i - 1):find("[%w_$.:]") ~= nil
end

-- At position i (code, not a string or comment): an `app.NAME(` call of a
-- decoration records its module in `out`. Returns the next position.
local function scan_decoration(text, i, out)
    if not ident_before(text, i) then
        local s, e, name = text:find("^app%s*[%.:]%s*([%a_][%w_]*)%s*%(", i)
        if s and DECORATIONS[name] then
            out[DECORATIONS[name]] = true
            return e + 1
        end
    end
    return i + 1
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
            -- `require ( "hull.X" )` or `require "hull.X"`, at a word start.
            local s, e, q, modname
            if not ident_before(text, i) then
                s, e, q, modname = text:find(
                    "^require%s*%(%s*([\"'])hull%.([%w%._%-]+)%1%s*%)", i)
                if not s then
                    s, e, q, modname = text:find("^require%s*([\"'])hull%.([%w%._%-]+)%1", i)
                end
            end
            if s and q then
                -- Normalize dots → slashes for canonical form.
                out[(("hull/" .. modname):gsub("%.", "/"))] = true
                i = e + 1
            else
                i = scan_decoration(text, i, out)
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
    local out_skip = false
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
            -- `import ... from "hull:X"` - capture only the module string.
            -- Handles `import x from`, `import {x} from`, `import * as x from`.
            -- A TypeScript `import type ... from` is erased, so it needs no
            -- declaration: skipped.
            local s, e, modname
            if not ident_before(text, i) then
                s, e, modname = text:find(
                    "^import[%s%w_${},*]+from%s*[\"']hull:([%w_:%-]+)[\"']", i)
                if s and text:find("^import%s+type[%s{*]", i) then
                    out_skip = true
                end
                if not s then
                    -- Dynamic `import("hull:X")`
                    s, e, modname = text:find(
                        "^import%s*%(%s*[\"']hull:([%w_:%-]+)[\"']%s*%)", i)
                end
            end
            if s then
                if not out_skip then
                    out[(("hull/" .. modname):gsub(":", "/"))] = true
                end
                out_skip = false
                i = e + 1
            else
                i = scan_decoration(text, i, out)
            end
        end
    end
    return out
end

-- ── Manifest module collection ──────────────────────────────────────

-- The registry, by name (tool.modules_available: the same table the
-- resolver and the loaders use).
local function registry()
    local reg = {}
    for _, spec in ipairs(tool.modules_available().modules) do reg[spec.name] = spec end
    return reg
end

-- The registered module a used name belongs to: itself, else its nearest
-- registered parent (hull/crypto/sealbox -> hull/crypto), as the loader
-- gates a sub-module import; nil when none is registered.
local function registered(reg, name)
    while name and not reg[name] do name = name:match("^(.+)/[^/]+$") end
    return name
end

-- The manifest's top-level declarations: { ["hull/crypto"] = true, ... }.
-- The array form holds specs ("hull/crypto@1", "hull/gpu@1?"); the legacy
-- keyed form holds them in its values.
local function collect_declared(manifest)
    local out = {}
    if not manifest or type(manifest.modules) ~= "table" then return out end
    for _, spec in pairs(manifest.modules) do
        if type(spec) == "string" then
            out[(spec:gsub("@.*$", ""):gsub("%?$", ""))] = true
        end
    end
    return out
end

-- ── Comparison ──────────────────────────────────────────────────────

-- used_by_file: { { path, set }, ... }; admitted: the resolver's set.
-- Returns undeclared = { { path, module }, ... }, unused = { name, ... }.
local function diff_sets(reg, declared, admitted, used_by_file)
    local undeclared = {}
    local used_anywhere = {}
    for _, e in ipairs(used_by_file) do
        local seen = {}
        for name, _ in pairs(e.set) do
            local mod = registered(reg, name) or name
            used_anywhere[mod] = true
            if not admitted[mod] and not seen[mod] then
                seen[mod] = true
                undeclared[#undeclared + 1] = { path = e.path, module = mod }
            end
        end
    end
    local unused = {}
    for name, _ in pairs(declared) do
        -- An intrinsic is always admitted, declared or not.
        if not used_anywhere[name] and not (reg[name] and reg[name].intrinsic) then
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

    -- Load app to extract manifest. App load can fail in tool-mode for
    -- legitimate reasons (apps that require relative JSON files via
    -- `require("./data.json")` - the tool VM lacks the real
    -- app-context's filesystem-fallback wiring). Treat it as
    -- "skipping analysis" rather than a hard failure so `hull check`
    -- still runs the rest of its pipeline. A genuine import problem
    -- still surfaces at server startup via the resolver. A missing entry
    -- is an error. The entry is app.lua, app.js or a TypeScript one, each
    -- read by its own runtime (an app.js was run through the Lua one).
    local fcompose = require("hull.feature_compose")
    local found, runtime = fcompose.app_entry(app_dir)
    if not found then
        tool.stderr("hull modules analyze: " .. runtime .. "\n")
        tool.exit(1)
    end
    local manifest, run_err = fcompose.app_manifest(app_dir, "hull modules analyze")
    if run_err then
        tool.stderr("hull modules analyze: app load error (" ..
                    run_err .. ") - skipping\n")
        return
    end
    local declared = collect_declared(manifest)
    -- What the app may use: the module resolver's set for this manifest (the
    -- declared modules, their dependencies, the intrinsics), as the runtime
    -- admits it. A manifest it refuses fails the app's load too. Resolved
    -- with the composable features' capabilities (gpu, tui), which `hull
    -- build` composes into the app whether or not this hull has them: the
    -- question is what the app declares, not what this binary can run.
    local reg = registry()
    local resolved = tool.modules_resolve(manifest or {}, nil, { "gpu", "tui" })
    if not resolved.ok then
        tool.stderr("hull modules analyze: " .. tostring(resolved.error) .. "\n")
        tool.exit(1)
    end
    local admitted = {}
    for _, m in ipairs(resolved.modules) do admitted[m.name] = true end

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

    local undeclared, unused = diff_sets(reg, declared, admitted, used)

    if want_json then
        -- Encoded JSON: written raw (json.encode escapes every C0 control
        -- itself; the escaping print would turn a raw DEL into \x7f).
        tool.stderr_raw(json.encode({
            app_dir    = app_dir,
            declared   = (function()
                local t = {}; for k, _ in pairs(declared) do t[#t+1] = k end
                table.sort(t); return t
            end)(),
            undeclared = undeclared,
            unused     = unused,
        }) .. "\n")
        if #undeclared > 0 then tool.exit(1) end
        return
    end

    -- Human-readable.
    if #undeclared == 0 and #unused == 0 then
        print("hull modules analyze: OK - every module used is declared, " ..
              "no unused declarations.")
        return
    end

    if #undeclared > 0 then
        print("Undeclared modules (the app fails to load, or the call raises):")
        for _, x in ipairs(undeclared) do
            print(string.format("  %s - %s", x.path, x.module))
        end
        print("")
        print("Fix: add the missing modules to app.manifest's `modules` array.")
        print("Example: modules = { \"" .. undeclared[1].module .. "@1\", ... }")
    end

    if #unused > 0 then
        if #undeclared > 0 then print("") end
        print("Declared but never used (capability surface larger than needed):")
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
