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

    -- app.lua, app.js or a TypeScript entry (run from the pipeline's erased
    -- modules); the app's top level runs in a runtime of its own.
    local m, err, entry = fcompose.app_manifest(app_dir, "hull manifest")
    if err then
        tool.stderr("hull manifest: " .. err .. "\n")
        tool.exit(1)
    end
    if not m then
        tool.stderr("hull manifest: no app.manifest() declared in " .. tool.escape(entry) .. "\n")
        tool.exit(1)
    end

    -- Print as JSON. Encoded, so written raw: json.encode escapes every C0
    -- control itself, and the escaping print would turn a DEL into \x7f.
    tool.stderr_json(json.encode(m) .. "\n")
end

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
