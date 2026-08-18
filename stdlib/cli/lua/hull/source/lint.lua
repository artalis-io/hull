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

-- ── rules (sorted by id; the registry order is deterministic) ─────────
M.RULES = {
    {
        id = "duplicate-table-key", severity = "warning", default = true,
        describe = "a table constructor with a repeated literal key",
        check = function(unit, emit)
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
        check = function(unit, emit)
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
        id = "todo-comment", severity = "info", default = true,
        describe = "a comment containing TODO / FIXME / XXX",
        check = function(unit, emit)
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

-- Run the enabled rules over a (cleanly-parsed) unit -> findings[] with each finding's
-- { code = "lua.lint.<id>", severity, rule = <id>, range, message }.
function M.run(unit, enabled)
    local findings = {}
    for _, rule in ipairs(M.RULES) do          -- registry order = deterministic
        if enabled[rule.id] then
            rule.check(unit, function(f)
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
