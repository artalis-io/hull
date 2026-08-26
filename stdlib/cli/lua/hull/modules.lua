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

local function find_entry(dir)
    local path = dir .. "/app.lua"
    if tool.file_exists(path) then return path end
    return nil
end

local function main()
    -- arg[0] is "list" (the subcommand). Scan the rest for --json and
    -- the first positional, which is the app dir. Don't break early -
    -- --json may appear after the positional.
    local want_json = false
    local app_dir
    for i = 1, #arg do
        if arg[i] == "--json" then
            want_json = true
        elseif arg[i]:sub(1, 1) ~= "-" and not app_dir then
            app_dir = arg[i]
        end
    end
    app_dir = app_dir or "."

    local entry = find_entry(app_dir)
    if not entry then
        tool.stderr("hull modules list: no app.lua found in " .. app_dir .. "\n")
        tool.exit(1)
    end

    local chunk, err = tool.loadfile(entry)
    if not chunk then
        tool.stderr("hull modules list: " .. tostring(err) .. "\n")
        tool.exit(1)
    end
    local ok, run_err = pcall(chunk)
    if not ok then
        tool.stderr("hull modules list: " .. tostring(run_err) .. "\n")
        tool.exit(1)
    end

    local m = app.get_manifest()
    if not m then
        tool.stderr("hull modules list: no app.manifest() declared in " .. entry .. "\n")
        tool.exit(1)
    end

    local modules = m.modules

    if want_json then
        -- Emit a stable object even when no modules are declared.
        print(json.encode({ modules = modules or {} }))
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
