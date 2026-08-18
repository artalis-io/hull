--
-- hull.project.inspect — `hull agent inspect [app_dir]`: standalone project discovery.
--
-- Slice 2 (design: docs/project_discovery_design.md D8/D9/D11). Invokes the ONE canonical
-- analyzer (hull.project.analyze) on the app tree and emits the shared public JSON
-- projection (hull.project.projection) to stdout. It does NOT re-scan or parse source
-- itself. This is the standalone path; the dev-running path (read a published generation)
-- lands in Slice 3 and reuses the SAME projection module.
--
-- Tool-module convention: this module RETURNS its main entry (like hull.build /
-- hull.init); the tool dispatcher (src/hull/tool.c) invokes it only when `inspect` is the
-- entry command. Requiring this module never runs the CLI.
--
-- Output: pure JSON on stdout (Hull routes `print` to stderr, so JSON goes via
-- tool.stdout). Exit 0 when a discovery was produced (validity is DATA in the JSON:
-- `valid` / `complete`); exit 2 on a usage error.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local analyze    = require("hull.project.analyze")
local projection = require("hull.project.projection")
local json       = require("hull.json")

local function usage_error(msg)
    tool.stderr("hull agent inspect: " .. msg .. "\n")
    tool.stderr("usage: hull agent inspect [app_dir]\n")
    tool.exit(2)
end

local function main()
    local positionals = {}
    for i = 1, #arg do
        local a = arg[i]
        if a == "-h" or a == "--help" then
            tool.stdout("usage: hull agent inspect [app_dir]\n"); tool.exit(0)
        elseif a:sub(1, 1) == "-" and a ~= "-" then
            -- `--json` is the default + only format, accepted for symmetry; any other flag errors.
            if a ~= "--json" then usage_error("unknown flag: " .. a) end
        else
            positionals[#positionals + 1] = a
        end
    end
    -- At most one positional (the app_dir). Reject `inspect a b` explicitly (exit 2)
    -- instead of silently using the last root.
    if #positionals > 1 then
        usage_error("expected at most one app_dir, got " .. #positionals ..
                    " (" .. table.concat(positionals, " ") .. ")")
    end
    local app_dir = positionals[1] or "."

    -- The canonical analyzer never raises: it always returns a discovery (an invalid one
    -- on a bad root / internal defect). The command therefore succeeds (exit 0); the
    -- consumer reads `valid` / `complete` from the JSON.
    local disc = analyze.analyze(app_dir, { source_kind = "standalone" })
    tool.stdout(json.encode(projection.project(disc)) .. "\n")
    tool.exit(0)
end

-- Hand main() back to the dispatcher; do NOT call it here (see the convention note above).
return main
