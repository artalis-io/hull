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

local function dirname(p) return p:match("^(.*)/[^/]*$") or "." end

-- Atomic publish: write a temp sibling then rename over the target (a reader never sees a
-- half-written discovery.json). Design D7. Returns true, or false + reason.
local function atomic_write(path, data)
    tool.mkdir(dirname(path))                         -- best-effort; fine if it exists
    local tmp = path .. ".tmp"
    if not tool.write_file(tmp, data) then return false, "write failed: " .. tmp end
    if not tool.rename(tmp, path) then tool.remove_file(tmp); return false, "rename failed" end
    return true
end

local function main()
    local positionals = {}
    local out_path, generation, session_pid = nil, 0, nil
    for i = 1, #arg do
        local a = arg[i]
        if a == "-h" or a == "--help" then
            tool.stdout("usage: hull agent inspect [app_dir]\n"); tool.exit(0)
        elseif a:match("^%-%-out=") then
            out_path = a:match("^%-%-out=(.+)$")
        elseif a:match("^%-%-generation=%d+$") then
            generation = tonumber(a:match("=(%d+)$"))
        elseif a:match("^%-%-session%-pid=%d+$") then
            session_pid = tonumber(a:match("=(%d+)$"))
        elseif a:sub(1, 1) == "-" and a ~= "-" then
            -- `--json` is the default + only stdout format, accepted for symmetry; else error.
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

    -- PUBLISH mode (`--out`, used by `hull dev`): a FRESH analysis (never the read path),
    -- projected + tagged with the dev generation + session identity, written atomically.
    if out_path then
        local disc = analyze.analyze(app_dir, { generation = generation, source_kind = "dev" })
        local p = projection.project(disc)
        p.session_pid = session_pid                   -- dev-session identity (D7); absent on standalone
        local okw, reason = atomic_write(out_path, json.encode(p) .. "\n")
        if not okw then
            tool.stderr("hull agent inspect: publish " .. tostring(reason) .. "\n"); tool.exit(2)
        end
        tool.exit(0)
    end

    -- STANDALONE mode. The canonical analyzer never raises: it always returns a discovery
    -- (an invalid one on a bad root / internal defect). Exit 0; the consumer reads
    -- `valid` / `complete` from the JSON. (The live-published read fast path is handled in
    -- C before this module is reached -- see cmd_inspect in commands/agent.c.)
    local disc = analyze.analyze(app_dir, { source_kind = "standalone" })
    tool.stdout(json.encode(projection.project(disc)) .. "\n")
    tool.exit(0)
end

-- Hand main() back to the dispatcher; do NOT call it here (see the convention note above).
return main
