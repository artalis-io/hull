--
-- hull.template -- Server-side template engine
--
-- template.render(name, data)             - load + compile + render (cached)
-- template.render_string(source, data)    - compile from string + render
-- template.compile(name)                  - returns compiled function
-- template.clear_cache()                  - clear compiled function cache
--
-- Syntax:
--   {{ var }}              HTML-escaped output
--   {{ var.path }}         dot path lookup (nil-safe)
--   {{ var | filter }}     pipe filter
--   {{ var | filter: arg }}filter with argument
--   {{{ var }}}            raw (unescaped) output
--   {% if var %}           conditional
--   {% if not var %}       negated conditional
--   {% elif var %}         else-if
--   {% elif not var %}     negated else-if
--   {% else %}             else
--   {% end %}              end block
--   {% for item in list %} iterate array
--   {% for key, val in obj %} iterate key/value pairs
--   {% block name %}       define overridable block
--   {% extends "name" %}   inherit from parent template
--   {% include "name" %}   include partial
--   {# comment #}         stripped from output
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local template = {}

-- ── HTML escape ──────────────────────────────────────────────────────

local escape_map = {
    ["&"] = "&amp;",
    ["<"] = "&lt;",
    [">"] = "&gt;",
    ['"'] = "&quot;",
    ["'"] = "&#39;",
}

local function html_escape(s)
    if s == nil then return "" end
    s = tostring(s)
    return (s:gsub("[&<>\"']", escape_map))
end

-- ── Built-in filters ────────────────────────────────────────────────

local filters = {}

function filters.upper(val)
    return tostring(val or ""):upper()
end

function filters.lower(val)
    return tostring(val or ""):lower()
end

function filters.trim(val)
    return tostring(val or ""):match("^%s*(.-)%s*$")
end

function filters.length(val)
    if type(val) == "table" then return #val end
    return #tostring(val or "")
end

function filters.default(val, fallback)
    if val == nil or val == false or val == "" then
        return fallback or ""
    end
    return val
end

function filters.json(val)
    return json.encode(val)
end

function filters.raw(val)
    return val
end

-- ── Lexer ────────────────────────────────────────────────────────────

-- Token types
local T_TEXT = "text"
local T_VAR = "var"           -- {{ expr }}
local T_RAW = "raw"           -- {{{ expr }}}
local T_TAG = "tag"           -- {% ... %}

local function lex(source)
    local tokens = {}
    local pos = 1
    local len = #source

    while pos <= len do
        -- Find next delimiter
        local d_start = source:find("{", pos)
        if not d_start then
            -- Rest is text
            if pos <= len then
                tokens[#tokens + 1] = { type = T_TEXT, value = source:sub(pos) }
            end
            break
        end

        -- Text before delimiter
        if d_start > pos then
            tokens[#tokens + 1] = { type = T_TEXT, value = source:sub(pos, d_start - 1) }
        end

        -- Determine delimiter type
        local c2 = source:sub(d_start, d_start + 2)
        local c1 = source:sub(d_start, d_start + 1)

        if c2 == "{{{" then
            -- Raw output: {{{ ... }}}
            local close = source:find("}}}", d_start + 3, true)
            if not close then
                error("unclosed {{{ at position " .. d_start)
            end
            local expr = source:sub(d_start + 3, close - 1):match("^%s*(.-)%s*$")
            tokens[#tokens + 1] = { type = T_RAW, value = expr }
            pos = close + 3
        elseif c1 == "{{" then
            -- Escaped output: {{ ... }}
            local close = source:find("}}", d_start + 2, true)
            if not close then
                error("unclosed {{ at position " .. d_start)
            end
            local expr = source:sub(d_start + 2, close - 1):match("^%s*(.-)%s*$")
            tokens[#tokens + 1] = { type = T_VAR, value = expr }
            pos = close + 2
        elseif c1 == "{%" then
            -- Tag: {% ... %}
            local close = source:find("%}", d_start + 2, true)
            if not close then
                error("unclosed {% at position " .. d_start)
            end
            local tag = source:sub(d_start + 2, close - 1):match("^%s*(.-)%s*$")
            tokens[#tokens + 1] = { type = T_TAG, value = tag }
            pos = close + 2
        elseif c1 == "{#" then
            -- Comment: {# ... #}
            local close = source:find("#}", d_start + 2, true)
            if not close then
                error("unclosed {# at position " .. d_start)
            end
            pos = close + 2
        else
            -- Not a template delimiter, just a lone {
            tokens[#tokens + 1] = { type = T_TEXT, value = "{" }
            pos = d_start + 1
        end
    end

    return tokens
end

-- ── Parser (recursive descent → AST) ────────────────────────────────

-- Parse a var expression: "name.path | filter: arg | filter2"
local function parse_expr(expr)
    local parts = {}
    for part in expr:gmatch("[^|]+") do
        parts[#parts + 1] = part:match("^%s*(.-)%s*$")
    end

    local var_part = parts[1]
    local filter_chain = {}

    for i = 2, #parts do
        local f = parts[i]
        local name, arg = f:match("^(%w+)%s*:%s*(.+)$")
        if not name then
            name = f:match("^(%w+)$")
        end
        if name then
            filter_chain[#filter_chain + 1] = { name = name, arg = arg }
        end
    end

    return { var = var_part, filters = filter_chain }
end

-- Parse tag content: "if cond", "for item in list", etc.
local function parse_tag(tag)
    -- extends "name"
    local extends_name = tag:match('^extends%s+"([^"]+)"$') or tag:match("^extends%s+'([^']+)'$")
    if extends_name then
        return { kind = "extends", name = extends_name }
    end

    -- include "name"
    local include_name = tag:match('^include%s+"([^"]+)"$') or tag:match("^include%s+'([^']+)'$")
    if include_name then
        return { kind = "include", name = include_name }
    end

    -- block name
    local block_name = tag:match("^block%s+(%S+)$")
    if block_name then
        return { kind = "block", name = block_name }
    end

    -- end
    if tag == "end" then
        return { kind = "end" }
    end

    -- else
    if tag == "else" then
        return { kind = "else" }
    end

    -- elif [not] cond
    local elif_neg, elif_cond = tag:match("^elif%s+(not)%s+(.+)$")
    if not elif_neg then
        elif_cond = tag:match("^elif%s+(.+)$")
    end
    if elif_cond then
        return { kind = "elif", cond = elif_cond, negated = elif_neg ~= nil }
    end

    -- if [not] cond
    local if_neg, if_cond = tag:match("^if%s+(not)%s+(.+)$")
    if not if_neg then
        if_cond = tag:match("^if%s+(.+)$")
    end
    if if_cond then
        return { kind = "if", cond = if_cond, negated = if_neg ~= nil }
    end

    -- for key, val in expr
    local fk, fv, fexpr = tag:match("^for%s+(%w+)%s*,%s*(%w+)%s+in%s+(.+)$")
    if fk then
        return { kind = "for_kv", key = fk, val = fv, expr = fexpr }
    end

    -- for item in expr
    local fvar, fexpr2 = tag:match("^for%s+(%w+)%s+in%s+(.+)$")
    if fvar then
        return { kind = "for", var = fvar, expr = fexpr2 }
    end

    error("unknown tag: {% " .. tag .. " %}")
end

local function parse(tokens)
    local ast = {}
    local pos = 1

    local function parse_body(stop_kinds)
        local body = {}
        while pos <= #tokens do
            local tok = tokens[pos]

            if tok.type == T_TEXT then
                body[#body + 1] = { kind = "text", value = tok.value }
                pos = pos + 1

            elseif tok.type == T_VAR then
                local expr = parse_expr(tok.value)
                body[#body + 1] = { kind = "var", expr = expr }
                pos = pos + 1

            elseif tok.type == T_RAW then
                local expr = parse_expr(tok.value)
                body[#body + 1] = { kind = "raw", expr = expr }
                pos = pos + 1

            elseif tok.type == T_TAG then
                local tag = parse_tag(tok.value)
                pos = pos + 1

                if tag.kind == "end" or tag.kind == "else" or tag.kind == "elif" then
                    if stop_kinds then
                        for _, k in ipairs(stop_kinds) do
                            if k == tag.kind then
                                return body, tag
                            end
                        end
                    end
                    error("unexpected {% " .. tag.kind .. " %}")
                end

                if tag.kind == "extends" then
                    body[#body + 1] = { kind = "extends", name = tag.name }

                elseif tag.kind == "include" then
                    body[#body + 1] = { kind = "include", name = tag.name }

                elseif tag.kind == "block" then
                    local block_body, _ = parse_body({"end"})
                    body[#body + 1] = { kind = "block", name = tag.name, body = block_body }

                elseif tag.kind == "if" then
                    local branches = {}
                    local else_body = nil

                    -- Parse the if-body; stop on end/else/elif
                    local if_body, stop = parse_body({"end", "else", "elif"})
                    branches[1] = { cond = tag.cond, negated = tag.negated, body = if_body }

                    -- Chain elif branches
                    while stop and stop.kind == "elif" do
                        local elif_cond = stop.cond
                        local elif_neg = stop.negated
                        local elif_body
                        elif_body, stop = parse_body({"end", "else", "elif"})
                        branches[#branches + 1] = { cond = elif_cond, negated = elif_neg, body = elif_body }
                    end

                    -- Optional else branch
                    if stop and stop.kind == "else" then
                        else_body = parse_body({"end"})
                    end

                    body[#body + 1] = { kind = "if", branches = branches, else_body = else_body }

                elseif tag.kind == "for" then
                    local for_body, _ = parse_body({"end"})
                    body[#body + 1] = { kind = "for", var = tag.var, expr = tag.expr, body = for_body }

                elseif tag.kind == "for_kv" then
                    local for_body, _ = parse_body({"end"})
                    body[#body + 1] = { kind = "for_kv", key = tag.key, val = tag.val, expr = tag.expr, body = for_body }
                end
            end
        end

        return body, nil
    end

    ast = parse_body(nil)
    return ast
end

-- ── Inheritance + include resolution ────────────────────────────────

-- Collect block overrides from child AST
local function collect_blocks(ast)
    local blocks = {}
    for _, node in ipairs(ast) do
        if node.kind == "block" then
            blocks[node.name] = node.body
        end
    end
    return blocks
end

-- Find extends node
local function find_extends(ast)
    for _, node in ipairs(ast) do
        if node.kind == "extends" then
            return node.name
        end
    end
    return nil
end

-- Apply block overrides from child into parent AST
local function apply_blocks(ast, overrides)
    local result = {}
    for _, node in ipairs(ast) do
        if node.kind == "block" then
            if overrides[node.name] then
                -- Replace with child's block content
                for _, child_node in ipairs(overrides[node.name]) do
                    result[#result + 1] = child_node
                end
            else
                -- Keep parent default
                for _, default_node in ipairs(node.body) do
                    result[#result + 1] = default_node
                end
            end
        else
            result[#result + 1] = node
        end
    end
    return result
end

-- Resolve inheritance chain
local function resolve_inheritance(ast, load_fn, visited)
    visited = visited or {}
    local parent_name = find_extends(ast)
    if not parent_name then
        return ast
    end

    if visited[parent_name] then
        error("circular extends: " .. parent_name)
    end
    visited[parent_name] = true

    local parent_source = load_fn(parent_name)
    if not parent_source then
        error("template not found: " .. parent_name)
    end

    local parent_tokens = lex(parent_source)
    local parent_ast = parse(parent_tokens)

    -- Recursively resolve parent's inheritance
    parent_ast = resolve_inheritance(parent_ast, load_fn, visited)

    -- Collect child blocks and apply to parent
    local child_blocks = collect_blocks(ast)
    return apply_blocks(parent_ast, child_blocks)
end

-- Resolve includes (inline)
local function resolve_includes(ast, load_fn, visited)
    visited = visited or {}
    local result = {}
    for _, node in ipairs(ast) do
        if node.kind == "include" then
            if visited[node.name] then
                error("circular include: " .. node.name)
            end
            visited[node.name] = true

            local source = load_fn(node.name)
            if not source then
                error("template not found: " .. node.name)
            end
            local inc_tokens = lex(source)
            local inc_ast = parse(inc_tokens)
            inc_ast = resolve_includes(inc_ast, load_fn, visited)
            for _, inc_node in ipairs(inc_ast) do
                result[#result + 1] = inc_node
            end
        elseif node.kind == "if" then
            -- Resolve includes inside branches
            local new_branches = {}
            for _, branch in ipairs(node.branches) do
                new_branches[#new_branches + 1] = {
                    cond = branch.cond,
                    negated = branch.negated,
                    body = resolve_includes(branch.body, load_fn, visited)
                }
            end
            local new_else = node.else_body and resolve_includes(node.else_body, load_fn, visited) or nil
            result[#result + 1] = { kind = "if", branches = new_branches, else_body = new_else }
        elseif node.kind == "for" then
            result[#result + 1] = { kind = "for", var = node.var, expr = node.expr,
                                     body = resolve_includes(node.body, load_fn, visited) }
        elseif node.kind == "for_kv" then
            result[#result + 1] = { kind = "for_kv", key = node.key, val = node.val, expr = node.expr,
                                     body = resolve_includes(node.body, load_fn, visited) }
        elseif node.kind == "block" then
            result[#result + 1] = { kind = "block", name = node.name,
                                     body = resolve_includes(node.body, load_fn, visited) }
        else
            result[#result + 1] = node
        end
    end
    return result
end

-- ── Lua code generator ──────────────────────────────────────────────

-- Generate nil-safe dot path access for Lua
-- "user.name" → "(__d.user and __d.user.name)"
-- "title" → "__d.title"
-- If the first segment is a loop-local variable, use it directly instead of __d.
local function gen_dot_path(path, prefix, locals_set)
    prefix = prefix or "__d"
    local parts = {}
    for part in path:gmatch("[^.]+") do
        parts[#parts + 1] = part
    end

    -- Check if first segment is a loop-local variable
    if locals_set and locals_set[parts[1]] then
        if #parts == 1 then
            return parts[1]
        end
        -- Build nil-safe chain using the local directly: item and item.name
        local checks = {}
        local chain = parts[1]
        for i = 2, #parts - 1 do
            chain = chain .. "." .. parts[i]
            checks[#checks + 1] = chain
        end
        chain = chain .. "." .. parts[#parts]
        if #checks == 0 then
            return "(" .. parts[1] .. " and " .. chain .. ")"
        end
        return "(" .. parts[1] .. " and " .. table.concat(checks, " and ") .. " and " .. chain .. ")"
    end

    if #parts == 1 then
        return prefix .. "." .. parts[1]
    end

    -- Build nil-safe chain: a and a.b and a.b.c
    local checks = {}
    local chain = prefix
    for i = 1, #parts - 1 do
        chain = chain .. "." .. parts[i]
        checks[#checks + 1] = chain
    end
    chain = chain .. "." .. parts[#parts]

    return "(" .. table.concat(checks, " and ") .. " and " .. chain .. ")"
end

-- Generate expression with filters
local function gen_expr(expr_info, escaped, locals_set)
    local code = gen_dot_path(expr_info.var, nil, locals_set)

    -- Apply filter chain
    for _, f in ipairs(expr_info.filters) do
        if f.name == "raw" then
            escaped = false
        elseif f.arg then
            -- Filter with argument
            local arg = f.arg:match("^%s*(.-)%s*$")
            -- Check if arg is a string literal
            if arg:sub(1, 1) == '"' or arg:sub(1, 1) == "'" then
                code = "__f." .. f.name .. "(" .. code .. ", " .. arg .. ")"
            else
                -- It's a variable reference
                code = "__f." .. f.name .. "(" .. code .. ", " .. gen_dot_path(arg, nil, locals_set) .. ")"
            end
        else
            code = "__f." .. f.name .. "(" .. code .. ")"
        end
    end

    if escaped then
        code = "__e(" .. code .. ")"
    end

    return code
end

-- Generate condition expression
local function gen_cond(cond, negated, locals_set)
    local path = gen_dot_path(cond:match("^%s*(.-)%s*$"), nil, locals_set)
    if negated then
        return "not " .. path
    end
    return path
end

local function lua_quote(s)
    -- Use Lua long string to avoid escaping issues
    -- Find a level of [=...=[ that doesn't appear in the string
    local level = 0
    while s:find("%]" .. string.rep("=", level) .. "%]") do
        level = level + 1
    end
    local eq = string.rep("=", level)
    return "[" .. eq .. "[" .. s .. "]" .. eq .. "]"
end

local function codegen(ast)
    local lines = {}
    local indent = 1
    local locals_set = {}  -- track loop-local variable names

    local function emit(line)
        lines[#lines + 1] = string.rep("  ", indent) .. line
    end

    local function gen_body(nodes)
        for _, node in ipairs(nodes) do
            if node.kind == "text" then
                if #node.value > 0 then
                    emit("__p[#__p+1] = " .. lua_quote(node.value))
                end

            elseif node.kind == "var" then
                emit("__p[#__p+1] = " .. gen_expr(node.expr, true, locals_set))

            elseif node.kind == "raw" then
                emit("__p[#__p+1] = tostring(" .. gen_expr(node.expr, false, locals_set) .. " or \"\")")

            elseif node.kind == "if" then
                for i, branch in ipairs(node.branches) do
                    if i == 1 then
                        emit("if " .. gen_cond(branch.cond, branch.negated, locals_set) .. " then")
                    else
                        emit("elseif " .. gen_cond(branch.cond, branch.negated, locals_set) .. " then")
                    end
                    indent = indent + 1
                    gen_body(branch.body)
                    indent = indent - 1
                end
                if node.else_body then
                    emit("else")
                    indent = indent + 1
                    gen_body(node.else_body)
                    indent = indent - 1
                end
                emit("end")

            elseif node.kind == "for" then
                emit("for _, " .. node.var .. " in ipairs(" .. gen_dot_path(node.expr, nil, locals_set) .. " or {}) do")
                locals_set[node.var] = true
                indent = indent + 1
                gen_body(node.body)
                indent = indent - 1
                locals_set[node.var] = nil
                emit("end")

            elseif node.kind == "for_kv" then
                emit("for " .. node.key .. ", " .. node.val .. " in pairs(" .. gen_dot_path(node.expr, nil, locals_set) .. " or {}) do")
                locals_set[node.key] = true
                locals_set[node.val] = true
                indent = indent + 1
                gen_body(node.body)
                indent = indent - 1
                locals_set[node.key] = nil
                locals_set[node.val] = nil
                emit("end")

            elseif node.kind == "block" then
                -- Blocks are already resolved by inheritance; render their content
                gen_body(node.body)
            end
        end
    end

    lines[#lines + 1] = "return function(__d, __e, __f)"
    lines[#lines + 1] = "  local __p = {}"
    gen_body(ast)
    lines[#lines + 1] = "  return table.concat(__p)"
    lines[#lines + 1] = "end"

    return table.concat(lines, "\n")
end

-- ── Compile + cache ─────────────────────────────────────────────────

local cache = {}

local function load_raw(name)
    return _template._load_raw(name)
end

local function compile_source(source, name)
    local tokens = lex(source)
    local ast = parse(tokens)
    ast = resolve_inheritance(ast, load_raw)
    ast = resolve_includes(ast, load_raw)
    local code = codegen(ast)
    local chunk_name = name and ("=template:" .. name) or "=template"
    local fn = _template._compile(code, chunk_name)
    return fn
end

--- Compile a named template (from embedded entries or filesystem).
-- Returns a function(data) that renders the template.
function template.compile(name)
    if cache[name] then
        return cache[name]
    end

    local source = load_raw(name)
    if not source then
        error("template not found: " .. name)
    end

    local fn = compile_source(source, name)
    cache[name] = fn
    return fn
end

--- Render a named template with data.
-- Loads, compiles (cached), and renders in one call.
function template.render(name, data)
    local fn = template.compile(name)
    return fn(data or {}, html_escape, filters)
end

--- Compile and render a template from a source string.
function template.render_string(source, data)
    local fn = compile_source(source, nil)
    return fn(data or {}, html_escape, filters)
end

--- Clear the compiled function cache.
function template.clear_cache()
    cache = {}
end

return template
