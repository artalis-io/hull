--
-- hull.modules - List the modules an app declares.
--
-- Usage:
--   hull modules list [app_dir]
--
-- Loads the app entry point, captures app.manifest(), and prints the
-- `modules` block. Human-readable by default; `--json` (a global flag,
-- forwarded as the env var HULL_JSON=1) switches to JSON output.
--
-- Mirrors hull.manifest's tool layout - both delegate to the Lua tool
-- VM because extracting the manifest requires executing app code.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")
local fcompose = require("hull.feature_compose")

local function main()
    -- arg[0] is "list" (the subcommand). Scan the rest for --json and
    -- the first positional, which is the app dir. Don't break early -
    -- --json may appear after the positional.
    local want_json = false
    local exec_lint = false   -- --exec-lint: hull check's manifest step
    local app_dir
    for i = 1, #arg do
        if arg[i] == "--json" then
            want_json = true
        elseif arg[i] == "--exec-lint" then
            exec_lint = true
        elseif arg[i]:sub(1, 1) ~= "-" and not app_dir then
            app_dir = arg[i]
        end
    end
    app_dir = app_dir or "."

    -- app.lua, app.js or a TypeScript entry (it looked for app.lua alone, so
    -- a JavaScript app - and hull check, which starts here - always failed).
    local m, err, entry = fcompose.app_manifest(app_dir, "hull modules list")
    if err then
        tool.stderr("hull modules list: " .. err .. "\n")
        tool.exit(1)
    end
    if not m then
        tool.stderr("hull modules list: no app.manifest() declared in " .. tool.escape(entry) .. "\n")
        tool.exit(1)
    end

    if exec_lint then require("hull.exec_lint").report(m, "hull check") end

    local modules = m.modules

    if want_json then
        -- Emit a stable object even when no modules are declared.
        -- Encoded JSON: raw (json.encode escapes every C0 control itself).
        tool.stderr_json(json.encode({ modules = modules or {} }) .. "\n")
        return
    end

    if not modules or next(modules) == nil then
        print("(no modules declared in " .. entry .. ")")
        print("Add a `modules = { \"hull/...@1\", ... }` block to app.manifest({...}).")
        return
    end

    -- Collect specs. The manifest format is an array of spec strings
    -- (`modules = { "hull/crypto@1", ... }`). The legacy keyed form
    -- (`modules = { crypto = "hull/crypto@1" }`) is still parsed by
    -- the C extractor for back-compat; surface both shapes here.
    local specs = {}
    if #modules > 0 then
        for _, spec in ipairs(modules) do specs[#specs + 1] = spec end
    else
        for _, spec in pairs(modules) do specs[#specs + 1] = spec end
    end
    table.sort(specs)

    print("Modules declared in " .. entry .. ":")
    for _, spec in ipairs(specs) do
        print(string.format("  %s", tostring(spec)))
    end
end

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
