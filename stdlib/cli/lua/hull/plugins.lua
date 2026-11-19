-- hull.plugins - `hull plugins list|run [app_dir] [--json]`: an app's build
-- plugins (docs/buildcontext_design.md §9.2, §13.5).
--
--   list   the registered plugins: identity, language, input, claims
--   run    run each source plugin over its selection and commit its result
--          (a fresh committed result is reused: "cached"); print the outputs
--
--   generation  run them, then write the app's pipeline generation (their
--          generated outputs, include/hull/shared/generation.h) to stdout:
--          what `hull dev` and an app run from source take. Diagnostics go
--          to stderr. Internal; its format is not a public interface.
--
-- Nothing is written into the app: results live in the build result store
-- (hull cache kind build-results). Exit 0, 1 when a plugin failed (or, for
-- generation, its outputs cannot be one), 2 on a usage or registration error.

local json = require("hull.json")

local function usage(msg)
    if msg then tool.stderr("hull plugins: " .. msg .. "\n") end
    tool.stderr("usage: hull plugins list|run [app_dir] [--json]\n")
    tool.exit(2)
end

local function fail(msg)
    tool.stderr("hull plugins: " .. msg .. "\n")
    tool.exit(2)
end

local function parse()
    local o = { dir = "." }
    local positional = {}
    for i = 1, #arg do
        local a = arg[i]
        if a == "--json" then o.json = true
        elseif a == "--help" or a == "-h" then o.help = true
        elseif a:sub(1, 1) == "-" then usage("unknown flag: " .. a)
        else positional[#positional + 1] = a end
    end
    if o.help then
        tool.stdout("usage: hull plugins list|run [app_dir] [--json]\n" ..
                    "  list  the build plugins hull.build.json registers\n" ..
                    "  run   run them over their source selections\n")
        tool.exit(0)
    end
    o.verb = positional[1]
    if o.verb ~= "list" and o.verb ~= "run" and o.verb ~= "generation" then usage(o.verb and ("unknown verb: " .. o.verb) or nil) end
    if positional[3] then usage("one app directory at most") end
    o.dir = positional[2] or "."
    return o
end

local function claims(p)
    return table.concat(p.claims, " ")
end

local function list(root, o)
    local reg, err = require("hull.buildctx.registry").load(root)
    if not reg then fail(err) end
    if o.json then
        local out = {}
        for i, p in ipairs(reg.plugins) do
            out[i] = { identity = p.identity, path = p.path, language = p.language,
                       descriptor = p.descriptor, config = p.config,
                       claims = p.claims, bundled = p.bundled ~= nil or nil }
        end
        tool.stdout_raw(json.encode({ plugins = out, config_sha256 = reg.config_sha256 }) .. "\n")
        return 0
    end
    if #reg.plugins == 0 then
        tool.stdout("no build plugins (no hull.build.json, or it names none)\n")
        return 0
    end
    for _, p in ipairs(reg.plugins) do
        tool.stdout(p.identity .. "\n")
        tool.stdout("  " .. p.language .. (p.bundled and " (bundled)" or "") .. ", " .. p.descriptor.input .. " -> " ..
                    p.descriptor.output .. "\n")
        tool.stdout("  claims: " .. claims(p) .. "\n")
    end
    return 0
end

local function run(root, o)
    local results, err = require("hull.buildctx.pipeline").run(root)
    if not results then fail(err) end
    local failed = false
    for _, r in ipairs(results) do
        if r.status == "failed" then failed = true end
    end
    if o.json then
        tool.stdout_raw(json.encode({ results = results }) .. "\n")
        return failed and 1 or 0
    end
    -- A Hull route with nothing to do (no TypeScript in the app) is not news.
    local shown = {}
    for _, r in ipairs(results) do
        if not (r.route and r.status == "no_input") then shown[#shown + 1] = r end
    end
    if #shown == 0 then
        tool.stdout("no build plugins (no hull.build.json, or it names none)\n")
        return 0
    end
    for _, r in ipairs(shown) do
        local head = r.identity .. "  " .. r.status
        if r.units then head = head .. "  " .. r.units .. (r.units == 1 and " unit" or " units") end
        if r.error then head = head .. "  " .. r.error end
        tool.stdout(head .. "\n")
        if r.detail then tool.stdout("  " .. tostring(r.detail) .. "\n") end
        for _, out in ipairs(r.outputs or {}) do
            local name = type(out.descriptor) == "table" and out.descriptor.name or "?"
            if out.sha256 then
                tool.stdout(("  %s %s  sha256 %s  %d bytes\n"):format(out.declared, name, out.sha256, out.size))
            else
                tool.stdout(("  %s %s  %d files\n"):format(out.declared, name, #out.entries))
                for _, ent in ipairs(out.entries) do
                    tool.stdout(("    %s  sha256 %s  %d bytes\n"):format(ent.path, ent.sha256, ent.size))
                end
            end
        end
        for _, d in ipairs(r.diagnostics or {}) do
            local where = d.path and (d.path .. ": ") or ""
            tool.stdout(("  %s %s: %s%s\n"):format(d.severity, d.code, where, d.message))
        end
    end
    return failed and 1 or 0
end

local function sha256_hex(s)
    return require("hull.encoding").hex.encode(require("hull.crypto").sha256(s))
end

-- stdout carries only the bundle: everything else goes to stderr.
local function generation(root)
    local _, bundle, err, stage = require("hull.buildctx.generation").produce(root, {
        sha256_hex = sha256_hex,
        report = function(r, d)
            local where = d.path and (d.path .. ": ") or ""
            tool.stderr(("hull plugins: %s: %s %s: %s%s\n"):format(
                r.identity, d.severity, d.code, where, d.message))
        end,
    })
    if not bundle then
        if stage == "run" then fail(err) end
        tool.stderr("hull plugins: " .. err .. "\n")
        return 1
    end
    tool.stdout_raw(bundle)
    return 0
end

return function()
    local o = parse()
    local root = tool.realpath(o.dir)
    if not root then fail(o.dir .. ": not a directory") end
    local verb = o.verb == "list" and list or o.verb == "run" and run or generation
    local rc = verb(root, o)
    tool.exit(rc)
end
