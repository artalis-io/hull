-- hull.buildctx.generation - a pipeline generation from one pipeline run
-- (docs/dev_pipeline_generations_design.md §1.2): the generated outputs of
-- every committed result, named for the app files they shadow, as the bundle
-- an app run from source takes (include/hull/shared/generation.h).
--
-- Host-owned tool code; `hull plugins generation` calls it.
--
--   local gen = require("hull.buildctx.generation")
--   local g, err = gen.from_results(results, sha256_hex)
--   -- g = { entries = { { name, bytes, origin, position_map } }, inputs,
--   --       plan_hash, summary }, or nil, err (a failed plugin, two outputs
--   --       of one name, a name an app cannot be handed)
--   local bundle = gen.encode(g, sha256_hex)
--
--   -- or both, from a pipeline run over the app at `root`:
--   local g, bundle, err = gen.produce(root, { sha256_hex = ..., report = fn })
--   -- report(r, d) is called for each result's diagnostic d
--
-- Only generated outputs become entries (an artifact is not an app file). A
-- tree output's files are named <output>/<path>. The inputs are every
-- workspace path a result's dependency manifest observed: what `hull dev`
-- watches besides the app's own files.

local action = require("hull.buildctx.action")
local outputs = require("hull.buildctx.outputs")

local M = {}

M.MAGIC = "hull-generation 1 "
M.FORMAT = "hull-pipeline-generation"
M.VERSION = 1
M.NAME_MAX = 1024

local POSITION_MAPS = { none = true, identity = true }

-- A plugin's text (its error, a diagnostic, an output's name) on one line, as
-- tool.escape writes it: every caller prints what comes out of here, and the
-- writers keep LF, so a raw one forged output lines (audit 49 escaped it in
-- `hull plugins` only; audit 50).
local function one_line(v)
    return (tostring(v):gsub("[%c\127]", function(c)
        if c == "\t" then return c end
        if c == "\n" then return "\\n" end
        if c == "\r" then return "\\r" end
        return ("\\x%02x"):format(c:byte())
    end))
end

-- The rule the app process applies (hl_generation_name_ok), checked here
-- first so a plugin's bad name is a pipeline error that names it.
function M.name_ok(name)
    if type(name) ~= "string" or #name == 0 or #name > M.NAME_MAX then return false end
    if not outputs.canonical(name) or name:find("[\\:]") then return false end
    if name:sub(1, 5) == "hull." or name:sub(1, 5) == "hull:" then return false end
    local first = name:match("^([^/]*)")
    return first:lower() ~= ".hull"
end

function M.from_results(results, sha256_hex)
    local entries, by_name, seen_in, inputs, keys = {}, {}, {}, {}, {}
    local summary = { error = 0, warning = 0, info = 0 }
    for _, r in ipairs(results) do
        if r.status == "failed" then
            return nil, one_line(r.identity) .. ": " .. one_line(r.error) ..
                        (r.detail and (" (" .. one_line(r.detail) .. ")") or "")
        end
        for _, d in ipairs(r.diagnostics or {}) do
            if summary[d.severity] then summary[d.severity] = summary[d.severity] + 1 end
        end
        local res = r.result
        if res then
            keys[#keys + 1] = { plugin = r.identity, key = r.key }
            local paths = action.observed_paths(res.deps)
            if not paths then return nil, one_line(r.identity) .. ": corrupt dependency manifest" end
            for _, p in ipairs(paths) do
                if not seen_in[p] then
                    seen_in[p] = true
                    inputs[#inputs + 1] = p
                end
            end
            for _, o in ipairs(res.outputs) do
                local desc = type(o.descriptor) == "table" and o.descriptor or {}
                if o.declared == "generated" then
                    local base = tostring(desc.name or "")
                    if base:sub(1, 2) == "./" then base = base:sub(3) end
                    local pm = desc.position_map or "none"
                    if not POSITION_MAPS[pm] then
                        return nil, one_line(r.identity) .. ": " .. one_line(base) ..
                                    ": position_map must be " ..
                                    "\"none\" or \"identity\""
                    end
                    local files = {}
                    if o.bytes then
                        files[1] = { name = base, bytes = o.bytes }
                    else
                        for _, ent in ipairs(o.entries) do
                            files[#files + 1] = { name = base .. "/" .. ent.path, bytes = ent.bytes }
                        end
                    end
                    for _, f in ipairs(files) do
                        if not M.name_ok(f.name) then
                            return nil, one_line(r.identity) .. ": " .. one_line(f.name) ..
                                        " is not a name an app file can have"
                        end
                        if by_name[f.name] then
                            return nil, one_line(f.name) .. " is generated by both " ..
                                        one_line(by_name[f.name]) .. " and " .. one_line(r.identity)
                        end
                        by_name[f.name] = r.identity
                        entries[#entries + 1] = {
                            name = f.name, bytes = f.bytes, position_map = pm,
                            origin = { plugin = r.identity, key = r.key },
                        }
                    end
                end
            end
        end
    end
    table.sort(entries, function(a, b) return a.name < b.name end)
    table.sort(inputs)
    local plan = outputs.encode({ format = "hull-pipeline-plan", version = 1, actions = keys })
    if not plan then return nil, "unencodable plan" end
    return { entries = entries, inputs = inputs, plan_hash = sha256_hex(plan), summary = summary }
end

-- An array as canonical JSON, empty included (outputs.encode writes an empty
-- table as an object).
local function array(items)
    local parts = {}
    for i, v in ipairs(items) do
        parts[i] = outputs.encode(v)
        if not parts[i] then return nil end
    end
    return "[" .. table.concat(parts, ",") .. "]"
end

function M.encode(g, sha256_hex)
    local list = {}
    for i, e in ipairs(g.entries) do
        list[i] = { vfs_name = e.name, content_hash = sha256_hex(e.bytes), size = #e.bytes,
                    origin = e.origin, position_map = e.position_map }
    end
    local ents, ins = array(list), array(g.inputs)
    local summary, id = outputs.encode(g.summary), outputs.encode(g.plan_hash)
    if not (ents and ins and summary and id) then
        return nil, "unencodable generation manifest"
    end
    local man = '{"diagnostics_summary":' .. summary .. ',"entries":' .. ents ..
                ',"format":' .. outputs.encode(M.FORMAT) .. ',"generation_id":' .. id ..
                ',"inputs":' .. ins .. ',"pipeline_plan_hash":' .. id ..
                ',"version":' .. M.VERSION .. "}"
    local parts = { M.MAGIC, tostring(#man), "\n", man }
    for _, e in ipairs(g.entries) do parts[#parts + 1] = e.bytes end
    return table.concat(parts)
end

-- Run the pipeline over the app at `root` and make its generation:
-- g, bundle, or nil, nil, err, stage ("run": the pipeline could not run -
-- a registry or discovery error; "generation": a plugin failed or its
-- outputs make no generation). `hull plugins generation` and `hull build`
-- (which embeds the entries) call it. opts: sha256_hex (required), report
-- (r, d) for each diagnostic, and pipeline.run's own options. report is
-- handed r.identity and the diagnostic's fields one_line'd, and every error
-- returned is one line.
function M.produce(root, opts)
    local sha256_hex = opts.sha256_hex
    local results, err = require("hull.buildctx.pipeline").run(root, opts)
    if not results then return nil, nil, one_line(err), "run" end
    if opts.report then
        for _, r in ipairs(results) do
            local rv = setmetatable({ identity = one_line(r.identity) }, { __index = r })
            for _, d in ipairs(r.diagnostics or {}) do
                opts.report(rv, {
                    severity = one_line(d.severity), code = one_line(d.code),
                    path = d.path ~= nil and one_line(d.path) or nil,
                    message = one_line(d.message),
                })
            end
        end
    end
    local g, gerr = M.from_results(results, sha256_hex)
    if not g then return nil, nil, "the pipeline failed: " .. gerr, "generation" end
    local bundle, eerr = M.encode(g, sha256_hex)
    if not bundle then return nil, nil, one_line(eerr), "generation" end
    return g, bundle
end

return M
