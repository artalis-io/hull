--
-- hull.project.publish — the INTERNAL dev-generation publisher (not a user surface).
--
-- Spawned by `hull dev --agent` (via the C dispatcher when the publish flags are present)
-- to write a fresh discovery generation. Distinct from hull.project.inspect (the
-- user-facing read/standalone command) so the publish authority is not exposed as a
-- read-command flag. Design: docs/project_discovery_design.md D7.
--
-- Contract: `<app_dir> --generation=N --session-pid=P` -- BOTH flags required (partial
-- combinations are a usage error). Output is ALWAYS the canonical
-- <app_dir>/.hull/discovery.json (never a caller-supplied path); a fresh analysis
-- (source="dev"), projected via the shared hull.project.projection, tagged with the
-- generation + session identity, written ATOMICALLY (tmp + rename).
--
-- Tool-module convention: RETURNS main; the dispatcher invokes it only as the entry.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local analyze    = require("hull.project.analyze")
local projection = require("hull.project.projection")
local json       = require("hull.json")

local function usage_error(msg)
    tool.stderr("hull project publish: " .. msg .. "\n")
    tool.exit(2)
end

local function dirname(p) return p:match("^(.*)/[^/]*$") or "." end

-- Atomic write: temp sibling then rename over the target (a reader never sees a partial
-- discovery.json). Returns true, or false + reason.
local function atomic_write(path, data)
    tool.mkdir(dirname(path))                        -- best-effort; fine if it exists
    local tmp = path .. ".tmp"
    if not tool.write_file(tmp, data) then return false, "write failed: " .. tmp end
    if not tool.rename(tmp, path) then tool.remove_file(tmp); return false, "rename failed" end
    return true
end

local function main()
    local positionals = {}
    local generation, session_pid = nil, nil
    for i = 1, #arg do
        local a = arg[i]
        if a:match("^%-%-generation=%d+$") then
            generation = tonumber(a:match("=(%d+)$"))
        elseif a:match("^%-%-session%-pid=%d+$") then
            session_pid = tonumber(a:match("=(%d+)$"))
        elseif a:sub(1, 1) == "-" and a ~= "-" then
            usage_error("unknown flag: " .. a)
        else
            positionals[#positionals + 1] = a
        end
    end
    if #positionals > 1 then
        usage_error("expected at most one app_dir, got " .. #positionals)
    end
    -- Both publish flags are required together (reject partial combinations).
    if not generation or not session_pid then
        usage_error("--generation and --session-pid are both required")
    end
    local app_dir = positionals[1] or "."

    local disc = analyze.analyze(app_dir, { generation = generation, source_kind = "dev" })
    local p = projection.project(disc)
    p.session_pid = session_pid                      -- dev-session identity (D7)

    -- CANONICAL output only -- derived from app_dir, never a caller-supplied path.
    local path = app_dir .. "/.hull/discovery.json"
    local okw, reason = atomic_write(path, json.encode(p) .. "\n")
    if not okw then usage_error("publish: " .. tostring(reason)) end
    tool.exit(0)
end

return main
