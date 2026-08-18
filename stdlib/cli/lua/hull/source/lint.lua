--
-- hull.source.lint — the lint rule registry + engine for `hull analyze` v2.
--
-- Each rule is a small declarative record; check(unit, emit) walks the AST / comments
-- (via hull.source.lua) and calls emit{ range, message } per finding. The engine tags
-- each finding with `lua.lint.<id>` + the rule's severity. Pure: no I/O, no cross-file
-- state (per-file rules). Design: docs/hull_analyze_lint_design.md.
--
-- SLICE 1 (this file): the engine + STRUCTURAL rules (no scope pass yet):
-- empty-block, duplicate-table-key, todo-comment. Scope-backed rules (unused-local,
-- shadowed-local, ...) land in slices 2-3 on hull.source.scope.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lua = require("hull.source.lua")

local M = {}

-- ── rule helpers ──────────────────────────────────────────────────────
-- Content of a SHORT string literal with no escapes, else nil (conservative: a long
-- string or an escaped one is not statically compared). Used to unify `a = 1` with
-- `["a"] = 1` (the same table key in Lua).
local function short_string_content(lit)
    if type(lit) ~= "table" or lit.kind ~= "literal" or lit.subtype ~= "string" then return nil end
    return lit.text:match('^"([^"\\]*)"$') or lit.text:match("^'([^'\\]*)'$")
end

-- Normalized static key of a table field, or nil when not determinable (a positional
-- item, a non-literal `[expr]`, or an escaped/long string key).
local function table_key(field)
    if field.kind == "field_name" then
        return "s:" .. field.name
    elseif field.kind == "field_expr" and type(field.key) == "table" then
        local k = field.key
        if k.kind == "literal" then
            if k.subtype == "string" then
                local c = short_string_content(k)
                return c and ("s:" .. c) or nil
            elseif k.subtype == "number" then
                return "n:" .. (k.text or "")
            end
        end
    end
    return nil
end

-- An idiomatic "ignore" name (`_`, `_x`) is exempt from unused/shadow rules.
local function is_ignored(name) return type(name) == "string" and name:sub(1, 1) == "_" end

-- EVIDENCE-BASED app-runtime global allowlist for undefined-global (design §5).
-- Derived from Hull's sandbox + registration, NOT guessed:
--   * Lua base globals surviving hl_lua_sandbox (runtime/lua/runtime.c removes
--     io/os/load/loadfile/dofile/package/debug) + surviving library tables;
--   * Hull app/test globals: app + hull (modules.c), require (mod_fs.c), test (mod_test.c).
-- Deliberately absent (so they correctly fire): req/res (params), db/fs/http/json/log/
-- crypto/compute/gpu (imported locals), the sandbox-removed names, and tool/arg
-- (tool VM). A future --tool-mode profile adds the tool-VM globals.
local GLOBAL_ALLOWLIST = {}
for _, g in ipairs({
    "assert", "collectgarbage", "error", "getmetatable", "ipairs", "next", "pairs",
    "pcall", "print", "rawequal", "rawget", "rawlen", "rawset", "select", "setmetatable",
    "tonumber", "tostring", "type", "warn", "xpcall", "_G", "_VERSION",
    "coroutine", "math", "string", "table", "utf8",
    "app", "hull", "require", "test",
}) do GLOBAL_ALLOWLIST[g] = true end

-- ── rules (sorted by id; the registry order is deterministic) ─────────
-- check(unit, scope, emit): `scope` is the hull.source.scope model (nil for a rule that
-- does not need it, or when resolution failed -- a needs_scope rule is then skipped).
M.RULES = {
    {
        id = "duplicate-table-key", severity = "warning", default = true,
        describe = "a table constructor with a repeated literal key",
        check = function(unit, _scope, emit)
            lua.walk(unit.ast, function(n)
                if n.kind ~= "table" then return end
                local seen = {}
                for _, f in ipairs(n.fields or {}) do
                    local key = table_key(f)
                    if key then
                        if seen[key] then emit({ range = f.range, message = "duplicate table key" })
                        else seen[key] = true end
                    end
                end
            end)
        end,
    },
    {
        id = "empty-block", severity = "warning", default = true,
        describe = "a control-flow block with an empty body",
        check = function(unit, _scope, emit)
            lua.walk(unit.ast, function(n)
                local k = n.kind
                if (k == "do" or k == "while" or k == "repeat"
                    or k == "numeric_for" or k == "generic_for")
                    and type(n.body) == "table" and #n.body == 0 then
                    emit({ range = n.range, message = "empty " .. k:gsub("_", " ") .. " block" })
                elseif k == "if" and type(n.clauses) == "table" then
                    for _, c in ipairs(n.clauses) do
                        if type(c.body) == "table" and #c.body == 0 then
                            emit({ range = n.range, message = "empty if branch" }); break
                        end
                    end
                end
            end)
        end,
    },
    {
        id = "shadowed-local", severity = "warning", default = true, needs_scope = true,
        describe = "a declaration that shadows an enclosing binding of the same name",
        check = function(_unit, scope, emit)
            for _, d in ipairs(scope.bindings) do
                if d.shadows and not d.implicit and not is_ignored(d.name) then
                    emit({ range = d.range, message = "'" .. d.name .. "' shadows an outer declaration" })
                end
            end
        end,
    },
    {
        id = "todo-comment", severity = "info", default = true,
        describe = "a comment containing TODO / FIXME / XXX",
        check = function(unit, _scope, emit)
            for _, c in ipairs(unit.comments or {}) do
                local text = c.text or ""
                for _, m in ipairs({ "TODO", "FIXME", "XXX" }) do
                    if text:find(m, 1, true) then
                        emit({ range = c.range, message = m .. " comment" }); break
                    end
                end
            end
        end,
    },
    {
        id = "undefined-global", severity = "warning", default = false, needs_scope = true,
        describe = "a read of a global not in Hull's app-runtime allowlist",
        check = function(_unit, scope, emit)
            for node, res in pairs(scope.ref_of) do
                if res.kind == "global" and res.access == "read"
                    and not GLOBAL_ALLOWLIST[node.name] then
                    emit({ range = node.range, message = "undefined global '" .. node.name .. "'" })
                end
            end
        end,
    },
    {
        id = "unused-local", severity = "warning", default = true, needs_scope = true,
        describe = "a local (or local function) that is never read",
        check = function(_unit, scope, emit)
            for _, d in ipairs(scope.bindings) do
                if (d.kind == "local" or d.kind == "localfunc") and d.reads == 0
                    and not is_ignored(d.name) then
                    emit({ range = d.range, message = "unused local '" .. d.name .. "'" })
                end
            end
        end,
    },
    {
        id = "unused-param", severity = "warning", default = true, needs_scope = true,
        describe = "a function parameter that is never read",
        check = function(_unit, scope, emit)
            for _, d in ipairs(scope.bindings) do
                if d.kind == "param" and d.reads == 0 and not d.implicit
                    and not is_ignored(d.name) then
                    emit({ range = d.range, message = "unused parameter '" .. d.name .. "'" })
                end
            end
        end,
    },
}

-- id -> rule
local BY_ID = {}
for _, r in ipairs(M.RULES) do BY_ID[r.id] = r end

function M.exists(id) return BY_ID[id] ~= nil end
function M.get(id) return BY_ID[id] end

-- The default-on set (a fresh table each call).
function M.default_enabled()
    local e = {}
    for _, r in ipairs(M.RULES) do if r.default then e[r.id] = true end end
    return e
end

-- Does any enabled rule require the scope model? (drives whether the analyzer computes
-- it; a needs_scope rule is skipped if scope is unavailable.)
function M.needs_scope(enabled)
    for _, rule in ipairs(M.RULES) do
        if enabled[rule.id] and rule.needs_scope then return true end
    end
    return false
end

-- Run the enabled rules over a (cleanly-parsed) unit -> findings[], each with
-- { code = "lua.lint.<id>", severity, rule = <id>, range, message }. `scope` is the
-- hull.source.scope model (or nil); a needs_scope rule is skipped when scope is nil.
function M.run(unit, enabled, scope)
    local findings = {}
    for _, rule in ipairs(M.RULES) do          -- registry order = deterministic
        if enabled[rule.id] and (not rule.needs_scope or scope) then
            rule.check(unit, scope, function(f)
                f.code = "lua.lint." .. rule.id
                f.severity = rule.severity
                f.rule = rule.id
                findings[#findings + 1] = f
            end)
        end
    end
    return findings
end

return M
