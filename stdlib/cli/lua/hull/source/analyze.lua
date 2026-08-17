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

-- generated / dependency dirs (build covers site/build). EXCLUDE_LIST prunes them
-- DURING traversal (tool.find_files exclude_dirs); EXCLUDE_SEGMENT is a cheap belt on
-- the returned paths.
local EXCLUDE_LIST = { ".git", ".hull", "build", "vendor", "node_modules" }
local EXCLUDE_SEGMENT = {}
for _, s in ipairs(EXCLUDE_LIST) do EXCLUDE_SEGMENT[s] = true end

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

-- Containment on CANONICAL absolute paths (from tool.realpath, symlinks resolved).
-- Handles the `/` root correctly (prefix is "/", not "//").
local function inside_root(canon_root, canon_target)
    if canon_root == canon_target then return true end
    local prefix = (canon_root == "/") and "/" or (canon_root .. "/")
    return canon_target:sub(1, #prefix) == prefix
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
        elseif a:match("^%-%-max%-depth=%d+$") then o.max_depth = tonumber(a:match("=(%d+)"))
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
    -- exclude_dirs prunes DURING traversal (a large build/ tree is never walked);
    -- find_files already returns sorted/regular/no-symlink. excluded() is a belt.
    local out, seen = {}, {}
    for _, p in ipairs(tool.find_files(root, "*.lua", { exclude_dirs = EXCLUDE_LIST })) do
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
        tool.stdout("usage: hull analyze [app_dir] [files...] [--json] [--quiet] [--max-depth=N]\n" ..
            "  static syntax analysis of an app's Lua source (parses, never runs).\n" ..
            "  --max-depth=N   cap parse nesting (default 2000); a deeper file is reported incomplete.\n")
        tool.exit(0)
    end
    if o.max_depth then LIMITS.max_depth = o.max_depth end   -- controlled low limit (testing / huge files)
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
        -- Canonicalize the root and each target (symlinks resolved) so containment is
        -- checked on the REAL location: a symlink whose spelling is inside the root but
        -- which resolves outside must be rejected. The user-facing LOGICAL path is kept
        -- for diagnostics; dedup is by canonical path (or logical when it doesn't resolve).
        local canon_root = tool.realpath(inp.root) or root_norm
        local seen = {}
        for _, raw in ipairs(inp.targets) do
            local logical = normalize(join(inp.root, raw))
            local canon, reason = tool.realpath(logical)
            local key = canon or logical
            if seen[key] then goto continue end
            seen[key] = true
            if canon == nil then                             -- distinguish missing vs inaccessible
                if reason == "missing" then
                    target_error(logical, "analyze.not_found", "no such file")
                else
                    target_error(logical, "analyze.unreadable",
                        "cannot access file (" .. tostring(reason) .. ")")
                end
            elseif not inside_root(canon_root, canon) then
                target_error(logical, "analyze.outside_root", "path resolves outside the app root")
            else
                local kind = tool.path_kind(canon)           -- canon exists -> dir/file/other
                if kind ~= "file" then
                    target_error(logical, "analyze.not_regular", "not a regular file (" .. tostring(kind) .. ")")
                elseif not ends_lua(logical) then
                    target_error(logical, "analyze.not_lua", "not a Lua (.lua) file")
                else
                    local src = tool.read_file(canon)
                    if not src then
                        target_error(logical, "analyze.unreadable", "cannot read file")
                    else
                        record(logical, analyze_source(logical, src))
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
