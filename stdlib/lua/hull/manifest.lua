--
-- hull.manifest — Extract and display app manifest as JSON
--
-- Usage: hull manifest [app_dir]
--
-- Executes the app entry point, captures app.manifest() declaration,
-- and prints it as formatted JSON to stdout.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")

local function find_entry(dir)
    -- Look for app.lua in the given directory
    local path = dir .. "/app.lua"
    local f = io.open(path, "r")
    if f then
        f:close()
        return path
    end
    return nil
end

local function main()
    local app_dir = arg[1] or "."

    local entry = find_entry(app_dir)
    if not entry then
        io.stderr:write("hull manifest: no app.lua found in " .. app_dir .. "\n")
        os.exit(1)
    end

    -- Execute the app file to capture manifest
    local chunk, err = loadfile(entry)
    if not chunk then
        io.stderr:write("hull manifest: " .. err .. "\n")
        os.exit(1)
    end

    local ok, run_err = pcall(chunk)
    if not ok then
        io.stderr:write("hull manifest: " .. tostring(run_err) .. "\n")
        os.exit(1)
    end

    -- Retrieve the manifest
    local m = app.get_manifest()
    if not m then
        io.stderr:write("hull manifest: no app.manifest() declared in " .. entry .. "\n")
        os.exit(1)
    end

    -- Print as JSON
    print(json.encode(m))
end

main()
