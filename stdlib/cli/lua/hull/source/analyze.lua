--
-- hull.source.analyze - `hull analyze`: static SYNTAX analysis of an app's Lua source.
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
local lint = require("hull.source.lint")
local scope = require("hull.source.scope")
local discover_mod = require("hull.source.discover")   -- the shared hardened walker (D2)
local json = require("hull.json")

-- Generous limits so lua.limit.* only trips on genuinely pathological input; a trip is
-- reported as an "incomplete" analysis (never silently treated as clean).
local LIMITS = { max_bytes = 64 * 1024 * 1024, max_tokens = 5000000,
                 max_comments = 5000000, max_depth = 2000 }

-- Path helpers + the exclusion set now live in the shared hardened walker
-- (hull.source.discover, D2); alias the ones the files-mode path still uses.
local normalize   = discover_mod.normalize
local join        = discover_mod.join
local inside_root = discover_mod.inside_root

local function ends_lua(path) return path:sub(-4) == ".lua" end

-- ── argument parsing (arg[0] = "analyze"; real args at arg[1..#arg]) ──
local function usage_error(msg)
    tool.stderr("hull analyze: " .. msg .. "\n")
    tool.stderr("usage: hull analyze [app_dir] [files...] [--json] [--quiet]\n")
    tool.exit(2)
end

-- Operational / discovery failure: exit 2, stderr only (JSON stdout stays pure). Used
-- to FAIL CLOSED -- never a fallback to lexical containment or a reduced scan.
local function op_fail(msg)
    tool.stderr("hull analyze: " .. msg .. "\n")
    tool.exit(2)
end

local function split_csv(s)
    local out = {}
    for part in s:gmatch("[^,]+") do out[#out + 1] = part end
    return out
end

local function parse_args()
    local o = { json = false, quiet = false, strict = false, positionals = {} }
    for i = 1, #arg do
        local a = arg[i]
        if a == "--json" then o.json = true
        elseif a == "--quiet" then o.quiet = true
        elseif a == "--strict" then o.strict = true
        elseif a == "--list-rules" then o.list_rules = true
        elseif a == "-h" or a == "--help" then o.help = true
        elseif a:match("^%-%-max%-depth=%d+$") then o.max_depth = tonumber(a:match("=(%d+)"))
        elseif a:match("^%-%-rules=.+$") then o.rules = split_csv(a:match("=(.+)$"))
        elseif a:match("^%-%-disable=.+$") then o.disable = split_csv(a:match("=(.+)$"))
        elseif a:match("^%-%-enable=.+$") then o.enable = split_csv(a:match("=(.+)$"))
        elseif a:sub(1, 1) == "-" and a ~= "-" then usage_error("unknown flag: " .. a)
        else o.positionals[#o.positionals + 1] = a end
    end
    return o
end

-- Resolve the active lint-rule set from --rules / --disable / --enable, validating
-- every referenced id (an unknown rule is a usage error). --rules restricts to exactly
-- that set; otherwise the default-on set minus --disable plus --enable.
local function resolve_rules(o)
    local function check_ids(list)
        for _, id in ipairs(list or {}) do
            if not lint.exists(id) then usage_error("unknown lint rule: " .. id) end
        end
    end
    check_ids(o.rules); check_ids(o.disable); check_ids(o.enable)
    local enabled
    if o.rules then
        enabled = {}
        for _, id in ipairs(o.rules) do enabled[id] = true end
    else
        enabled = lint.default_enabled()
        for _, id in ipairs(o.disable or {}) do enabled[id] = nil end
        for _, id in ipairs(o.enable or {}) do enabled[id] = true end
    end
    return enabled
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

-- ── discovery (walk mode): the shared hardened walker, .lua only for `hull analyze` ──
-- A discovery error (OOM / access) is an OPERATIONAL failure (op_fail exit 2), not an
-- empty scan -- the shared module returns (nil, err) and this CLI decides fail-closed.
local function discover(root)
    local files, err = discover_mod.discover(root)           -- default ext {"lua"}, DEFAULT_EXCLUDE
    if err then op_fail("discovery failed: " .. tostring(err)) end
    return files
end

-- ── analyze one readable Lua file: returns (state, diagnostics[]) ──
-- Syntax diagnostics are severity "error"; lint findings (only on a CLEANLY-parsed
-- file) carry their rule's severity (warning / info).
local function analyze_source(path, src, enabled)
    local unit, err = lua.parse(src, { path = path, limits = LIMITS })
    if unit == nil then                                      -- (nil, err): API/internal failure
        local msg = (type(err) == "table" and err.message) or tostring(err)
        local code = (type(err) == "table" and err.code) or "lua.internal"
        return "internal", { { code = code, severity = "error", message = msg } }
    end
    local diags, has_limit, has_internal, has_syntax = {}, false, false, false
    for _, d in ipairs(unit.diagnostics) do
        local code = d.code or ""
        if code:find("^lua%.limit%.") then has_limit = true
        elseif code == "lua.internal" then has_internal = true
        else has_syntax = true end                           -- lua.syntax / lua.unsupported
        local line, col
        if d.range then line, col = unit:line_col(d.range) end
        diags[#diags + 1] = {
            code = code, severity = "error", message = d.message or "",
            range = d.range and { start = d.range.start, stop = d.range.stop } or nil,
            line = line, col = col,
        }
    end
    local state = has_internal and "internal" or (has_limit and "incomplete") or "complete"

    -- Lint only a cleanly-parsed file (complete + no syntax errors): a recovered,
    -- error-bearing AST would yield spurious lint findings.
    if state == "complete" and not has_syntax and enabled and next(enabled) then
        local sc = nil
        if lint.needs_scope(enabled) then
            local resolved, serr = scope.resolve(unit)
            if serr then
                -- resolver internal failure: surface it, downgrade the file to "internal"
                -- (so JSON files[].state + summary.internal stay consistent with the exit
                -- code), and SKIP scope-backed rules (sc stays nil). Structural rules,
                -- already independent of scope, still run below.
                state = "internal"
                local line, col
                if serr.range then line, col = unit:line_col(serr.range) end
                diags[#diags + 1] = {
                    code = serr.code, severity = serr.severity or "error", message = serr.message,
                    range = serr.range and { start = serr.range.start, stop = serr.range.stop } or nil,
                    line = line, col = col,
                }
            else
                sc = resolved
            end
        end
        for _, f in ipairs(lint.run(unit, enabled, sc)) do
            local line, col
            if f.range then line, col = unit:line_col(f.range) end
            diags[#diags + 1] = {
                code = f.code, severity = f.severity, message = f.message, rule = f.rule,
                range = f.range and { start = f.range.start, stop = f.range.stop } or nil,
                line = line, col = col,
            }
        end
    end
    return state, diags
end

-- ── --list-rules: enumerate the lint registry, then exit ──
local function list_rules(o)
    if o.json then
        local rules = {}
        for _, r in ipairs(lint.RULES) do
            rules[#rules + 1] = { id = r.id, severity = r.severity, default = r.default, description = r.describe }
        end
        tool.stdout(json.encode({ schema_version = 2, rules = rules }) .. "\n")
    else
        local out = {}
        for _, r in ipairs(lint.RULES) do
            out[#out + 1] = string.format("%-22s %-8s %-4s %s",
                r.id, r.severity, r.default and "on" or "off", r.describe)
        end
        tool.stdout(table.concat(out, "\n") .. "\n")
    end
    tool.exit(0)
end

-- ── build the result set: files[] + diagnostics[] + summary ──
local function run()
    local o = parse_args()
    if o.help then
        tool.stdout(
            "usage: hull analyze [app_dir] [files...] [--json] [--quiet] [--strict]\n" ..
            "                    [--rules=a,b] [--disable=c,d] [--enable=e] [--list-rules] [--max-depth=N]\n" ..
            "  static analysis of an app's Lua source (parses, never runs): syntax + lint rules.\n" ..
            "  --strict        warnings fail the run (exit 1); advisory by default.\n" ..
            "  --rules=a,b     run ONLY these lint rules;  --disable / --enable adjust the default set.\n" ..
            "  --list-rules    list the lint rules and exit.\n" ..
            "  --max-depth=N   cap parse nesting (default 2000); a deeper file is reported incomplete.\n")
        tool.exit(0)
    end
    if o.list_rules then list_rules(o) end
    local enabled = resolve_rules(o)                         -- validates --rules/--disable/--enable
    if o.max_depth then LIMITS.max_depth = o.max_depth end   -- controlled low limit (testing / huge files)
    local inp = resolve_inputs(o)
    local root_norm = normalize(inp.root)

    -- Collect { path, state, diags[] } records. A file with a target error carries an
    -- analyze.* diagnostic and state "internal"; a readable Lua file is parsed.
    local files, diagnostics = {}, {}

    local function record(path, state, diags)
        files[#files + 1] = { path = path, state = state }
        for _, d in ipairs(diags) do
            d.path = path; d.state = state                   -- severity is set per-diag already
            diagnostics[#diagnostics + 1] = d
        end
    end
    local function target_error(path, code, message)
        record(path, "internal", { { code = code, severity = "error", message = message } })
    end

    if inp.mode == "walk" then
        local paths = discover(inp.root)
        for _, path in ipairs(paths) do
            local src = tool.read_file(path)
            if not src then                                  -- fail closed: never a silent skip
                target_error(path, "analyze.unreadable", "cannot read file")
            else
                record(path, analyze_source(path, src, enabled))
            end
        end
    else                                                     -- explicit files
        -- Canonicalize the root and each target (symlinks resolved) so containment is
        -- checked on the REAL location: a symlink whose spelling is inside the root but
        -- which resolves outside must be rejected. The user-facing LOGICAL path is kept
        -- for diagnostics; dedup is by canonical path (or logical when it doesn't resolve).
        -- Root canonicalization failure is an OPERATIONAL error, never a fallback to
        -- lexical containment (which would let a symlink escape the root).
        local canon_root = tool.realpath(inp.root)
        if not canon_root then op_fail("cannot resolve app root: " .. inp.root) end
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
                        record(logical, analyze_source(logical, src, enabled))
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

    -- summary (per-severity; warnings are advisory unless --strict)
    local errors, warnings, infos, incomplete, internal, with_issues = 0, 0, 0, 0, 0, 0
    local by_rule, seen_issue = {}, {}
    for _, f in ipairs(files) do
        if f.state == "incomplete" then incomplete = incomplete + 1 end
        if f.state == "internal" then internal = internal + 1 end
    end
    for _, d in ipairs(diagnostics) do
        if d.severity == "error" then errors = errors + 1
        elseif d.severity == "warning" then warnings = warnings + 1
        elseif d.severity == "info" then infos = infos + 1 end
        if d.rule then by_rule[d.rule] = (by_rule[d.rule] or 0) + 1 end
        if not seen_issue[d.path] then seen_issue[d.path] = true; with_issues = with_issues + 1 end
    end
    -- exit 0 iff no errors / incomplete / internal, and (warnings advisory unless --strict)
    local exit_ok = (errors == 0 and incomplete == 0 and internal == 0
                     and (not o.strict or warnings == 0))
    local no_findings = (#diagnostics == 0 and incomplete == 0 and internal == 0)

    return o, inp, root_norm, files, diagnostics, {
        errors = errors, warnings = warnings, infos = infos,
        incomplete = incomplete, internal = internal,
        files_with_issues = with_issues, by_rule = by_rule,
        clean = exit_ok,                                     -- clean == exit 0 (design §8)
    }, no_findings
end

-- ── output (real output on STDOUT via tool.stdout; print is routed to stderr) ──
local function emit_json(root_norm, files, diagnostics, summary)
    tool.stdout(json.encode({
        schema_version = 2,                                  -- v2: lint findings + severities
        root = (root_norm == "") and "." or root_norm,
        files_scanned = #files,
        files = files,
        diagnostics = diagnostics,
        summary = summary,
    }) .. "\n")
end

local function pluralize(n, word)
    return n .. " " .. word .. (n == 1 and "" or "s")
end

local function emit_human(o, files, diagnostics, summary, no_findings)
    local out = {}
    for _, d in ipairs(diagnostics) do
        local pos = (d.line and d.col) and (d.line .. ":" .. d.col) or "?:?"
        out[#out + 1] = string.format("%s:%s: %s: %s [%s]", d.path, pos, d.severity, d.message, d.code)
    end
    if not o.quiet then                                      -- --quiet drops the summary chatter
        if no_findings then
            out[#out + 1] = string.format("hull analyze: no issues (%d files scanned)", #files)
        else
            local parts = {}
            if summary.errors > 0 then parts[#parts + 1] = pluralize(summary.errors, "error") end
            if summary.warnings > 0 then parts[#parts + 1] = pluralize(summary.warnings, "warning") end
            if summary.infos > 0 then parts[#parts + 1] = summary.infos .. " info" end
            if summary.incomplete > 0 then parts[#parts + 1] = summary.incomplete .. " incomplete" end
            if summary.internal > 0 then parts[#parts + 1] = summary.internal .. " internal" end
            if #diagnostics > 0 then out[#out + 1] = "" end
            out[#out + 1] = string.format("hull analyze: %s in %s (%d files scanned)",
                table.concat(parts, ", "), pluralize(summary.files_with_issues, "file"), #files)
        end
    end
    if #out > 0 then tool.stdout(table.concat(out, "\n") .. "\n") end
end

-- `analyze_source` is the pure core (parse + lint, no I/O). Expose it so the test
-- harness can drive the three-state contract (incl. an injected resolver failure)
-- without the CLI shell. The CLI entry runs ONLY in the tool VM, where `tool` is a
-- global; a plain `require` (test env, no `tool`) returns the module without exiting.
local M = { analyze_source = analyze_source }

if tool then
    local o, _, root_norm, files, diagnostics, summary, no_findings = run()
    if o.json then
        emit_json(root_norm, files, diagnostics, summary)    -- JSON overrides --quiet; stdout is pure JSON
    else
        emit_human(o, files, diagnostics, summary, no_findings)
    end
    tool.exit(summary.clean and 0 or 1)
end

return M
