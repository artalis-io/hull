--
-- hull.manifest - Extract and display app manifest as JSON
--
-- Usage: hull manifest [app_dir]
--
-- Executes the app entry point, captures app.manifest() declaration,
-- and prints it as formatted JSON to stdout.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")
local fcompose = require("hull.feature_compose")

local function main()
    local app_dir = arg[1] or "."

    -- The app's entry: app.lua, app.js, or a TypeScript entry, which runs
    -- from the pipeline's erased modules, so the pipeline runs first.
    local entry, kind = fcompose.app_entry(app_dir)
    if not entry then
        tool.stderr("hull manifest: " .. kind .. "\n")
        tool.exit(1)
    end
    local _, gen_err = fcompose.prepare_generation(app_dir, "hull manifest")
    if gen_err then
        tool.stderr("hull manifest: " .. gen_err .. "\n")
        tool.exit(1)
    end

    local m = nil
    if kind == "lua" then
        -- The app's top level runs in a Lua runtime of its own, never this
        -- tool VM (see hl_lua_extract_manifest_json).
        local json_s, run_err = tool.extract_manifest_lua(entry)
        if run_err then
            tool.stderr("hull manifest: " .. tostring(run_err) .. "\n")
            tool.exit(1)
        end
        if json_s then
            local decoded, decode_err = json.decode(json_s)
            if not decoded then
                tool.stderr("hull manifest: JSON decode failed: " .. tostring(decode_err) .. "\n")
                tool.exit(1)
            end
            m = decoded
        end
    else
        -- JS entry: spin up a transient JS runtime via the C-side
        -- helper, get back the JSON-stringified manifest, decode.
        local ok, json_or_err = pcall(tool.extract_manifest_js, entry,
                                      fcompose.generation(app_dir))
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

    -- Print as JSON. Encoded, so written raw: json.encode escapes every C0
    -- control itself, and the escaping print would turn a DEL into \x7f.
    tool.stderr_raw(json.encode(m) .. "\n")
end

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
