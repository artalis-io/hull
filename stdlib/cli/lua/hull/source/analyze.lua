--
-- hull.source.analyze — `hull analyze`: static SYNTAX analysis of an app's Lua source.
--
-- The first production consumer of hull.source.lua. Parses every .lua file in an app
-- (or explicit files) WITHOUT running or building it, and reports diagnostics with
-- exact path:line:col. Design + locked contract: docs/hull_analyze_design.md.
--
-- NOT to be confused with `hull modules analyze` (module hull.analyze), which compares
-- require/import sites against manifest.modules. This is source syntax analysis.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lua = require("hull.source.lua")
local json = require("hull.json")

-- Generous limits so lua.limit.* only trips on genuinely pathological input; a trip is
-- reported as an "incomplete" analysis (never silently treated as clean).
local LIMITS = { max_bytes = 64 * 1024 * 1024, max_tokens = 5000000,
                 max_comments = 5000000, max_depth = 2000 }

local EXCLUDE_SEGMENT = {   -- generated / dependency dirs (build covers site/build)
    [".git"] = true, [".hull"] = true, build = true, vendor = true, node_modules = true,
}

-- ── small path helpers (pure Lua; no path-normalize binding in the tool VM) ──
local function normalize(p)
    local absolute = p:sub(1, 1) == "/"
    local segs = {}
    for seg in p:gmatch("[^/]+") do
        if seg == ".." then
            if #segs > 0 and segs[#segs] ~= ".." then table.remove(segs)
            elseif not absolute then segs[#segs + 1] = ".." end   -- absolute: .. at root stays root
        elseif seg ~= "." then                               -- "." is dropped
            segs[#segs + 1] = seg
        end
    end
    return (absolute and "/" or "") .. table.concat(segs, "/")
end

local function join(root, rel)
    if rel:sub(1, 1) == "/" then return rel end              -- absolute stays absolute
    if root == "" or root == "." then return rel end
    return root .. "/" .. rel
end

-- Is `target_norm` inside `root_norm`? (root_norm "" means cwd.)
local function inside_root(root_norm, target_norm)
    if target_norm == ".." or target_norm:sub(1, 3) == "../" then return false end
    if root_norm == "" then return target_norm:sub(1, 1) ~= "/" end
    return target_norm == root_norm or target_norm:sub(1, #root_norm + 1) == root_norm .. "/"
end

local function excluded(path)
    for seg in path:gmatch("[^/]+") do
        if EXCLUDE_SEGMENT[seg] then return true end
    end
    return false
end

local function ends_lua(path) return path:sub(-4) == ".lua" end

-- ── argument parsing (arg[0] = "analyze"; real args at arg[1..#arg]) ──
local function usage_error(msg)
    tool.stderr("hull analyze: " .. msg .. "\n")
    tool.stderr("usage: hull analyze [app_dir] [files...] [--json] [--quiet]\n")
    tool.exit(2)
end

local function parse_args()
    local o = { json = false, quiet = false, positionals = {} }
    for i = 1, #arg do
        local a = arg[i]
        if a == "--json" then o.json = true
        elseif a == "--quiet" then o.quiet = true
        elseif a == "-h" or a == "--help" then o.help = true
        elseif a:sub(1, 1) == "-" and a ~= "-" then usage_error("unknown flag: " .. a)
        else o.positionals[#o.positionals + 1] = a end
    end
    return o
end

-- ── resolve inputs into { root, mode, targets? } ──
local function resolve_inputs(o)
    local pos = o.positionals
    if #pos == 0 then
        return { root = ".", mode = "walk" }
    end
    if tool.path_kind(pos[1]) == "dir" then                  -- first positional is app_dir
        local root = pos[1]
        if #pos == 1 then return { root = root, mode = "walk" } end
        local targets = {}
        for i = 2, #pos do targets[#targets + 1] = pos[i] end
        return { root = root, mode = "files", targets = targets }
    end
    return { root = ".", mode = "files", targets = pos }     -- all positionals are files under .
end

-- ── discovery (walk mode): sorted, regular .lua, no symlink, exclusions ──
local function discover(root)
    local out, seen = {}, {}
    for _, p in ipairs(tool.find_files(root, "*.lua")) do    -- already sorted/regular/no-symlink
        local n = normalize(p)
        if not excluded(n) and not seen[n] then
            seen[n] = true; out[#out + 1] = n
        end
    end
    table.sort(out)
    return out
end

-- ── analyze one readable Lua file: returns (state, diagnostics[]) ──
local function analyze_source(path, src)
    local unit, err = lua.parse(src, { path = path, limits = LIMITS })
    if unit == nil then                                      -- (nil, err): API/internal failure
        local msg = (type(err) == "table" and err.message) or tostring(err)
        local code = (type(err) == "table" and err.code) or "lua.internal"
        return "internal", { { code = code, message = msg } }
    end
    local diags, has_limit, has_internal = {}, false, false
    for _, d in ipairs(unit.diagnostics) do
        local code = d.code or ""
        if code:find("^lua%.limit%.") then has_limit = true
        elseif code == "lua.internal" then has_internal = true end
        local line, col
        if d.range then line, col = unit:line_col(d.range) end
        diags[#diags + 1] = {
            code = code, message = d.message or "",
            range = d.range and { start = d.range.start, stop = d.range.stop } or nil,
            line = line, col = col,
        }
    end
    local state = has_internal and "internal" or (has_limit and "incomplete") or "complete"
    return state, diags
end

-- ── build the result set: files[] + diagnostics[] + summary ──
local function run()
    local o = parse_args()
    if o.help then
        tool.stdout("usage: hull analyze [app_dir] [files...] [--json] [--quiet]\n" ..
            "  static syntax analysis of an app's Lua source (parses, never runs).\n")
        tool.exit(0)
    end
    local inp = resolve_inputs(o)
    local root_norm = normalize(inp.root)

    -- Collect { path, state, diags[] } records. A file with a target error carries an
    -- analyze.* diagnostic and state "internal"; a readable Lua file is parsed.
    local files, diagnostics = {}, {}

    local function record(path, state, diags)
        files[#files + 1] = { path = path, state = state }
        for _, d in ipairs(diags) do
            d.path = path; d.severity = "error"; d.state = state
            diagnostics[#diagnostics + 1] = d
        end
    end
    local function target_error(path, code, message)
        record(path, "internal", { { code = code, message = message } })
    end

    if inp.mode == "walk" then
        local paths = discover(inp.root)
        for _, path in ipairs(paths) do
            local src = tool.read_file(path)
            if not src then                                  -- fail closed: never a silent skip
                target_error(path, "analyze.unreadable", "cannot read file")
            else
                record(path, analyze_source(path, src))
            end
        end
    else                                                     -- explicit files
        local seen = {}
        for _, raw in ipairs(inp.targets) do
            local path = normalize(join(inp.root, raw))
            if seen[path] then goto continue end
            seen[path] = true
            if not inside_root(root_norm, path) then
                target_error(path, "analyze.outside_root", "path is outside the app root")
            else
                local kind = tool.path_kind(path)
                if kind == nil then
                    target_error(path, "analyze.not_found", "no such file")
                elseif kind ~= "file" then
                    target_error(path, "analyze.not_regular", "not a regular file (" .. kind .. ")")
                elseif not ends_lua(path) then
                    target_error(path, "analyze.not_lua", "not a Lua (.lua) file")
                else
                    local src = tool.read_file(path)
                    if not src then
                        target_error(path, "analyze.unreadable", "cannot read file")
                    else
                        record(path, analyze_source(path, src))
                    end
                end
            end
            ::continue::
        end
    end

    -- deterministic ordering
    table.sort(files, function(a, b) return a.path < b.path end)
    table.sort(diagnostics, function(a, b)
        if a.path ~= b.path then return a.path < b.path end
        local as, bs = (a.range and a.range.start) or 0, (b.range and b.range.start) or 0
        if as ~= bs then return as < bs end
        return a.code < b.code
    end)

    -- summary
    local errors, incomplete, internal, with_issues = 0, 0, 0, 0
    local seen_issue = {}
    for _, f in ipairs(files) do
        if f.state == "incomplete" then incomplete = incomplete + 1 end
        if f.state == "internal" then internal = internal + 1 end
    end
    for _, d in ipairs(diagnostics) do
        errors = errors + 1
        if not seen_issue[d.path] then seen_issue[d.path] = true; with_issues = with_issues + 1 end
    end
    local clean = (errors == 0 and incomplete == 0 and internal == 0)

    return o, inp, root_norm, files, diagnostics, {
        errors = errors, files_with_issues = with_issues,
        incomplete = incomplete, internal = internal, clean = clean,
    }
end

-- ── output (real output on STDOUT via tool.stdout; print is routed to stderr) ──
local function emit_json(root_norm, files, diagnostics, summary)
    tool.stdout(json.encode({
        schema_version = 1,
        root = (root_norm == "") and "." or root_norm,
        files_scanned = #files,
        files = files,
        diagnostics = diagnostics,
        summary = summary,
    }) .. "\n")
end

local function emit_human(o, files, diagnostics, summary)
    local out = {}
    for _, d in ipairs(diagnostics) do
        local pos = (d.line and d.col) and (d.line .. ":" .. d.col) or "?:?"
        out[#out + 1] = string.format("%s:%s: %s: %s [%s]", d.path, pos, d.severity, d.message, d.code)
    end
    if not o.quiet then                                      -- --quiet drops the summary chatter
        if summary.clean then
            out[#out + 1] = string.format("hull analyze: no issues (%d files scanned)", #files)
        else
            local parts = { summary.errors .. " error" .. (summary.errors == 1 and "" or "s") ..
                            " in " .. summary.files_with_issues .. " file" ..
                            (summary.files_with_issues == 1 and "" or "s") }
            if summary.incomplete > 0 then parts[#parts + 1] = summary.incomplete .. " incomplete" end
            if summary.internal > 0 then parts[#parts + 1] = summary.internal .. " internal" end
            if #diagnostics > 0 then out[#out + 1] = "" end
            out[#out + 1] = string.format("hull analyze: %s (%d files scanned)",
                table.concat(parts, ", "), #files)
        end
    end
    if #out > 0 then tool.stdout(table.concat(out, "\n") .. "\n") end
end

local o, _, root_norm, files, diagnostics, summary = run()
if o.json then
    emit_json(root_norm, files, diagnostics, summary)        -- JSON overrides --quiet; stdout is pure JSON
else
    emit_human(o, files, diagnostics, summary)
end
tool.exit(summary.clean and 0 or 1)
