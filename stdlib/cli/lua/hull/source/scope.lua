--
-- hull.source.scope — lexical binding / name-resolution pass (lint slice 2).
--
-- scope.resolve(unit) -> scope, err : resolves every `name` AST node to its binding
-- (a local/param/loopvar, an upvalue across a function boundary, or a global) and
-- records per-declaration reads/writes + shadowing. The reusable "Step B" that slice-3
-- scope-backed rules (unused-local, shadowed-local, ...) consume.
--
-- Design + Lua-5.4 scoping subtleties: docs/hull_source_scope_design.md. The traversal
-- is pcall-guarded: a recovered error node degrades LOCALLY, but an internal failure
-- returns (nil, err) so the caller surfaces an internal diagnostic and skips scope
-- rules -- never a silent partial model.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- The core traversal, in a fresh local state per call (reentrant). Returns the scope
-- model; raising is caught by M.resolve's pcall.
local function do_resolve(unit)
    local scopes = { { vars = {}, func_id = 1 } }   -- the chunk IS function scope 1
    local func_id_counter = 1
    local bindings, ref_of = {}, {}

    local function cur() return scopes[#scopes] end
    local function cur_func() return cur().func_id end

    local function push_block()
        scopes[#scopes + 1] = { vars = {}, func_id = cur_func() }
    end
    local function push_function()
        func_id_counter = func_id_counter + 1
        scopes[#scopes + 1] = { vars = {}, func_id = func_id_counter }
    end
    local function pop() scopes[#scopes] = nil end

    -- Add a declaration to the current scope; set `shadows` if a same-name binding is
    -- visible in the current scope or an enclosing block OF THE SAME FUNCTION (current
    -- scope included -> same-block redeclaration counts).
    local function add_decl(name, kind, range, implicit)
        local my_func = cur_func()
        local shadows = nil
        for i = #scopes, 1, -1 do
            local sc = scopes[i]
            if sc.func_id ~= my_func then break end   -- crossed a function boundary
            if sc.vars[name] then shadows = sc.vars[name]; break end
        end
        local decl = {
            name = name, kind = kind, range = range, implicit = implicit or nil,
            func_id = my_func, reads = 0, writes = 0, shadows = shadows,
        }
        cur().vars[name] = decl                       -- shadow any prior in this scope
        bindings[#bindings + 1] = decl
        return decl
    end

    -- Resolve a `name` node: nearest enclosing binding wins (local if in the current
    -- function, else upvalue); no binding -> global. Counts a read or a write.
    local function resolve_ref(node, is_write)
        local name = node.name
        for i = #scopes, 1, -1 do
            local decl = scopes[i].vars[name]
            if decl then
                if is_write then decl.writes = decl.writes + 1
                else decl.reads = decl.reads + 1 end
                ref_of[node] = { decl = decl, kind = (decl.func_id == cur_func()) and "local" or "upvalue" }
                return
            end
        end
        ref_of[node] = { kind = "global" }
    end

    local visit_expr, visit_stat, visit_block, visit_function

    function visit_expr(node)
        if type(node) ~= "table" or not node.kind then return end
        local k = node.kind
        if k == "name" then
            resolve_ref(node, false)                  -- a read (writes handled at target sites)
        elseif k == "field" then
            visit_expr(node.obj)                      -- .name is a key, not a ref
        elseif k == "index" then
            visit_expr(node.obj); visit_expr(node.key)
        elseif k == "call" then
            visit_expr(node.callee)
            for _, a in ipairs(node.args or {}) do visit_expr(a) end
        elseif k == "method_call" then
            visit_expr(node.obj)                      -- :method is a key
            for _, a in ipairs(node.args or {}) do visit_expr(a) end
        elseif k == "binary" then
            visit_expr(node.lhs); visit_expr(node.rhs)
        elseif k == "unary" then
            visit_expr(node.operand)
        elseif k == "paren" then
            visit_expr(node.expr)
        elseif k == "table" then
            for _, f in ipairs(node.fields or {}) do
                if f.kind == "field_expr" then visit_expr(f.key); visit_expr(f.value)
                else visit_expr(f.value) end          -- field_name key is not a ref; field_item value
            end
        elseif k == "function_expr" then
            visit_function(node, nil)
        end
        -- literal / vararg / error / unknown: no refs (degrade locally)
    end

    function visit_function(node, self_anchor)
        push_function()
        if self_anchor then add_decl("self", "param", self_anchor, true) end
        for _, p in ipairs(node.params or {}) do
            if p.kind == "param" and p.name then add_decl(p.name, "param", p.range) end
            -- a vararg param is not a name binding
        end
        visit_block(node.body or {})
        pop()
    end

    function visit_stat(s)
        if type(s) ~= "table" or not s.kind then return end
        local k = s.kind
        if k == "local_declaration" then
            for _, v in ipairs(s.values or {}) do visit_expr(v) end   -- values FIRST (local visible after)
            for _, nm in ipairs(s.names or {}) do
                if nm.name then add_decl(nm.name, "local", nm.range) end
            end
        elseif k == "function_declaration" then
            local nm = s.name
            if s.is_local then
                if nm and nm.kind == "name" then add_decl(nm.name, "localfunc", nm.range) end  -- visible in body
                visit_function(s, nil)
            else
                if nm and nm.kind == "name" then
                    resolve_ref(nm, true)             -- `function f()` = a write to the resolved f
                elseif nm and nm.kind == "field" then
                    local base = nm
                    while type(base) == "table" and base.kind == "field" do base = base.obj end
                    if type(base) == "table" and base.kind == "name" then resolve_ref(base, false) end
                end
                local anchor = (s.is_method and nm and nm.range) or nil
                visit_function(s, anchor)
            end
        elseif k == "assignment" then
            for _, v in ipairs(s.values or {}) do visit_expr(v) end
            for _, t in ipairs(s.targets or {}) do
                if type(t) == "table" and t.kind == "name" then resolve_ref(t, true)
                else visit_expr(t) end                -- field/index target: reads obj/key
            end
        elseif k == "call_statement" then
            visit_expr(s.call)
        elseif k == "do" then
            push_block(); visit_block(s.body or {}); pop()
        elseif k == "while" then
            visit_expr(s.cond)                        -- cond in the enclosing scope
            push_block(); visit_block(s.body or {}); pop()
        elseif k == "repeat" then
            push_block(); visit_block(s.body or {})
            visit_expr(s.cond)                        -- until: IN the body scope (Lua special case)
            pop()
        elseif k == "if" then
            for _, c in ipairs(s.clauses or {}) do
                if c.cond then visit_expr(c.cond) end -- cond in the enclosing scope
                push_block(); visit_block(c.body or {}); pop()
            end
        elseif k == "numeric_for" then
            visit_expr(s.from); visit_expr(s.to); if s.step then visit_expr(s.step) end
            push_block()
            if s.var and s.var.name then add_decl(s.var.name, "loopvar", s.var.range) end
            visit_block(s.body or {}); pop()
        elseif k == "generic_for" then
            for _, e in ipairs(s.exprs or {}) do visit_expr(e) end
            push_block()
            for _, nm in ipairs(s.names or {}) do
                if nm.name then add_decl(nm.name, "loopvar", nm.range) end
            end
            visit_block(s.body or {}); pop()
        elseif k == "return" then
            for _, v in ipairs(s.values or {}) do visit_expr(v) end
        end
        -- break / goto / label / error: no bindings or refs
    end

    function visit_block(stmts)
        for _, s in ipairs(stmts) do visit_stat(s) end
    end

    if type(unit) == "table" and type(unit.ast) == "table" then
        visit_block(unit.ast.body or {})
    end
    return { bindings = bindings, ref_of = ref_of }
end

-- Public boundary: (scope, err). err ~= nil (and scope == nil) ONLY on an internal
-- resolver failure -- a recovered/error-bearing AST degrades locally, not here.
function M.resolve(unit)
    local ok, result = pcall(do_resolve, unit)
    if not ok then
        return nil, { code = "lua.internal", message = "scope resolver failed: " .. tostring(result) }
    end
    return result, nil
end

return M
