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

-- Resolve which entry file holds the manifest. Apps are single-runtime;
-- only one of app.lua / app.js is meaningful in a given directory.
local function find_entry(dir)
    local lua_path = dir .. "/app.lua"
    if tool.file_exists(lua_path) then return lua_path, "lua" end
    local js_path = dir .. "/app.js"
    if tool.file_exists(js_path) then return js_path, "js" end
    return nil, nil
end

local function main()
    local app_dir = arg[1] or "."

    local entry, kind = find_entry(app_dir)
    if not entry then
        tool.stderr("hull manifest: no app.lua or app.js found in " .. app_dir .. "\n")
        tool.exit(1)
    end

    local m = nil
    if kind == "lua" then
        local chunk, err = tool.loadfile(entry)
        if not chunk then
            tool.stderr("hull manifest: " .. tostring(err) .. "\n")
            tool.exit(1)
        end

        local ok, run_err = pcall(chunk)
        if not ok then
            tool.stderr("hull manifest: " .. tostring(run_err) .. "\n")
            tool.exit(1)
        end

        m = app.get_manifest()
    else
        -- JS entry: spin up a transient JS runtime via the C-side
        -- helper, get back the JSON-stringified manifest, decode.
        local ok, json_or_err = pcall(tool.extract_manifest_js, entry)
        if not ok then
            tool.stderr("hull manifest: " .. tostring(json_or_err) .. "\n")
            tool.exit(1)
        end
        if json_or_err then
            local decoded, decode_err = json.decode(json_or_err)
            if not decoded then
                tool.stderr("hull manifest: JSON decode failed: " .. tostring(decode_err) .. "\n")
                tool.exit(1)
            end
            m = decoded
        end
    end

    if not m then
        tool.stderr("hull manifest: no app.manifest() declared in " .. entry .. "\n")
        tool.exit(1)
    end

    -- Print as JSON
    print(json.encode(m))
end

main()
