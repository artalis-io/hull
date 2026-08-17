--
-- hull.source.parser — recursive-descent Lua 5.4 parser (expressions + statements).
--
-- Consumes the lexer token stream and builds the Hull AST (expressions AND
-- statements) with EXACT half-open byte ranges (see range.lua). M.parse_chunk()
-- returns the whole-source { kind="chunk", body, range }; M.parse_expression()
-- parses a single expression. Node vocabulary matches
-- docs/lua_source_analysis_design.md §5.
--
-- The parser NEVER raises: a parse error emits a lua.syntax diagnostic and returns
-- an { kind = "error" } node (best-effort recovery -- a stall guard in block() and
-- a token-consuming primary() keep the chunk parsing past a bad construct).
-- Expression and block nesting share one max_depth budget (a terminal
-- lua.limit.max_depth diagnostic, no stack overflow); ordinary diagnostics respect
-- max_diagnostics while terminal ones always record.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local diag = require("hull.source.diagnostic")

local M = {}

-- Lua 5.4 binary operator priorities {left, right} (lparser.c). Right-assoc ops
-- (`..`, `^`) have right < left. `and`/`or` are keywords; the rest are ops. `~`
-- is binary xor here (unary bitwise-not is handled in prefix position).
local BINPRI = {
    ["or"] = { 1, 1 }, ["and"] = { 2, 2 },
    ["<"] = { 3, 3 }, [">"] = { 3, 3 }, ["<="] = { 3, 3 }, [">="] = { 3, 3 },
    ["~="] = { 3, 3 }, ["=="] = { 3, 3 },
    ["|"] = { 4, 4 }, ["~"] = { 5, 5 }, ["&"] = { 6, 6 },
    ["<<"] = { 7, 7 }, [">>"] = { 7, 7 },
    [".."] = { 9, 8 },
    ["+"] = { 10, 10 }, ["-"] = { 10, 10 },
    ["*"] = { 11, 11 }, ["/"] = { 11, 11 }, ["//"] = { 11, 11 }, ["%"] = { 11, 11 },
    ["^"] = { 14, 13 },
}
local UNARY_PRIORITY = 12
local UNARY = { ["not"] = true, ["-"] = true, ["#"] = true, ["~"] = true }

-- ── parser state ──────────────────────────────────────────────────────
local P = {}
P.__index = P

local function new_state(tokens, source, opts)
    local limits = opts.limits or {}
    return setmetatable({
        tokens = tokens, source = source, path = opts.path,
        diagnostics = {}, pos = 1,
        prev = nil,                         -- last consumed token (for node end offsets)
        depth = 0, max_depth = limits.max_depth or 400,
        max_diagnostics = limits.max_diagnostics or 200,
        aborted = false,
    }, P)
end

-- Normal diagnostics respect max_diagnostics; terminal (limit) diagnostics always
-- record, mirroring the lexer's policy so the reason parsing stopped is never
-- suppressed.
function P:emit(d) if #self.diagnostics < self.max_diagnostics then self.diagnostics[#self.diagnostics + 1] = d end end
function P:emit_terminal(d) self.diagnostics[#self.diagnostics + 1] = d end

function P:cur() return self.tokens[self.pos] end
function P:advance() local t = self.tokens[self.pos]; self.prev = t; self.pos = self.pos + 1; return t end
function P:at_eof() return self:cur().kind == "eof" end

function P:is_op(text) local t = self:cur(); return t.kind == "op" and t.text == text end
function P:is_kw(text) local t = self:cur(); return t.kind == "keyword" and t.text == text end

function P:err(message, tok)
    tok = tok or self:cur()
    self:emit(diag.error("lua.syntax", message, self.path, tok.range))
    return { kind = "error", range = { start = tok.range.start, stop = tok.range.stop } }
end

function P:unsupported(message, s, e)
    self:emit(diag.error("lua.unsupported", message, self.path, { start = s, stop = e }))
end

-- Consume `text` op/kw or emit a diagnostic; returns the token or nil.
function P:expect_op(text)
    if self:is_op(text) then return self:advance() end
    self:err("'" .. text .. "' expected"); return nil
end
function P:expect_kw(text)
    if self:is_kw(text) then return self:advance() end
    self:err("'" .. text .. "' expected"); return nil
end

-- node spanning [start_off, self.prev.stop)
function P:finish(node, start_off)
    node.range = { start = start_off, stop = (self.prev and self.prev.range.stop) or start_off }
    return node
end

-- ── the binary operator, if the current token is one ──────────────────
function P:binop()
    local t = self:cur()
    if t.kind == "op" and BINPRI[t.text] then return t.text end
    if t.kind == "keyword" and (t.text == "and" or t.text == "or") then return t.text end
    return nil
end

-- ── expression: precedence climbing (Lua's algorithm) ─────────────────
function P:subexpr(limit)
    self.depth = self.depth + 1
    if self.depth > self.max_depth then
        if not self.aborted then
            self.aborted = true
            self:emit_terminal(diag.error("lua.limit.max_depth",
                "expression nesting exceeds max_depth (" .. self.max_depth .. ")",
                self.path, self:cur().range))
        end
        self.depth = self.depth - 1
        return { kind = "error", range = self:cur().range }
    end

    local e
    local t = self:cur()
    if (t.kind == "op" and UNARY[t.text]) or (t.kind == "keyword" and t.text == "not") then
        local op = t.text
        local start = t.range.start
        self:advance()
        local operand = self:subexpr(UNARY_PRIORITY)
        e = self:finish({ kind = "unary", op = op, operand = operand }, start)
    else
        e = self:simple()
    end

    while not self.aborted do
        local op = self:binop()
        if not op or BINPRI[op][1] <= limit then break end
        self:advance()
        local rhs = self:subexpr(BINPRI[op][2])
        e = self:finish({ kind = "binary", op = op, lhs = e, rhs = rhs }, e.range.start)
    end

    self.depth = self.depth - 1
    return e
end


-- ── simple expressions ────────────────────────────────────────────────
function P:simple()
    local t = self:cur()
    local start = t.range.start

    if t.kind == "number" then
        self:advance()
        return self:finish({ kind = "literal", subtype = "number", text = t.text, malformed = t.malformed }, start)
    elseif t.kind == "string" then
        self:advance()
        return self:finish({ kind = "literal", subtype = "string", text = t.text, malformed = t.malformed }, start)
    elseif t.kind == "keyword" and (t.text == "nil" or t.text == "true" or t.text == "false") then
        self:advance()
        -- CONVENTION: every literal carries `text` = its exact lexeme. Booleans
        -- ADDITIONALLY expose the actual boolean `value`; nil carries no `value`.
        if t.text == "nil" then
            return self:finish({ kind = "literal", subtype = "nil", text = t.text }, start)
        end
        return self:finish({ kind = "literal", subtype = "boolean", text = t.text,
                             value = (t.text == "true") }, start)
    elseif self:is_op("...") then
        self:advance()
        return self:finish({ kind = "vararg" }, start)
    elseif self:is_op("{") then
        return self:table_constructor()
    elseif t.kind == "keyword" and t.text == "function" then
        return self:function_expr()
    else
        return self:suffixed()
    end
end

-- ── prefix / suffixed expressions (var, call, index, method) ──────────
function P:primary()
    local t = self:cur()
    local start = t.range.start
    if self:is_op("(") then
        self:advance()
        local inner = self:subexpr(0)
        self:expect_op(")")
        return self:finish({ kind = "paren", expr = inner }, start)
    elseif t.kind == "name" then
        self:advance()
        return self:finish({ kind = "name", name = t.text }, start)
    else
        local e = self:err("unexpected symbol")
        if not self:at_eof() then self:advance() end   -- consume to progress (avoid stalls)
        return e
    end
end

function P:suffixed()
    local e = self:primary()
    local start = e.range.start
    while true do
        if self:is_op(".") then
            self:advance()
            local name = self:cur()
            if name.kind ~= "name" then self:err("<name> expected after '.'"); break end
            self:advance()
            e = self:finish({ kind = "field", obj = e, name = name.text }, start)
        elseif self:is_op("[") then
            self:advance()
            local key = self:subexpr(0)
            self:expect_op("]")
            e = self:finish({ kind = "index", obj = e, key = key }, start)
        elseif self:is_op(":") then
            self:advance()
            local method = self:cur()
            if method.kind ~= "name" then self:err("<name> expected after ':'"); break end
            self:advance()
            local args = self:call_args()
            e = self:finish({ kind = "method_call", obj = e, method = method.text, args = args }, start)
        elseif self:is_op("(") or self:is_op("{") or self:cur().kind == "string" then
            local args = self:call_args()
            e = self:finish({ kind = "call", callee = e, args = args }, start)
        else
            break
        end
    end
    return e
end

-- args := '(' [explist] ')' | tableconstructor | String
function P:call_args()
    if self:cur().kind == "string" then
        local t = self:advance()
        return { self:finish({ kind = "literal", subtype = "string", text = t.text, malformed = t.malformed }, t.range.start) }
    elseif self:is_op("{") then
        return { self:table_constructor() }
    elseif self:is_op("(") then
        self:advance()
        local args = {}
        if not self:is_op(")") then
            args[#args + 1] = self:subexpr(0)
            while self:is_op(",") do self:advance(); args[#args + 1] = self:subexpr(0) end
        end
        self:expect_op(")")
        return args
    else
        self:err("function arguments expected")
        return {}
    end
end

-- ── table constructor ─────────────────────────────────────────────────
-- field := '[' exp ']' '=' exp | Name '=' exp | exp ; sep := ',' | ';'
function P:table_constructor()
    local start = self:cur().range.start
    self:expect_op("{")
    local fields = {}
    while not self:is_op("}") and not self:at_eof() do
        local fstart = self:cur().range.start
        if self:is_op("[") then
            self:advance()
            local key = self:subexpr(0)
            self:expect_op("]")
            self:expect_op("=")
            local val = self:subexpr(0)
            fields[#fields + 1] = self:finish({ kind = "field_expr", key = key, value = val }, fstart)
        elseif self:cur().kind == "name" and self.tokens[self.pos + 1]
               and self.tokens[self.pos + 1].kind == "op" and self.tokens[self.pos + 1].text == "=" then
            local nametok = self:advance()
            self:advance()                          -- '='
            local val = self:subexpr(0)
            fields[#fields + 1] = self:finish({ kind = "field_name", name = nametok.text, value = val }, fstart)
        else
            local val = self:subexpr(0)
            fields[#fields + 1] = self:finish({ kind = "field_item", value = val }, fstart)
        end
        if self:is_op(",") or self:is_op(";") then self:advance() else break end
    end
    self:expect_op("}")
    return self:finish({ kind = "table", fields = fields }, start)
end

-- ── function parameters / body (shared by function_expr, function decl) ─
-- funcparams := '(' [ Name {',' Name} [',' '...'] | '...' ] ')'
function P:funcparams()
    self:expect_op("(")
    local params, is_vararg = {}, false
    if not self:is_op(")") then
        while true do
            if self:is_op("...") then
                local v = self:advance(); is_vararg = true
                params[#params + 1] = { kind = "vararg", range = v.range }
                break
            elseif self:cur().kind == "name" then
                local nm = self:advance()
                params[#params + 1] = { kind = "param", name = nm.text, range = nm.range }
                if self:is_op(",") then self:advance() else break end
            else
                self:err("<name> or '...' expected in parameter list"); break
            end
        end
    end
    self:expect_op(")")
    return params, is_vararg
end

-- funcbody := '(' params ')' block 'end'   (the real block parser -- slice 3)
function P:funcbody()
    local params, is_vararg = self:funcparams()
    local body = self:block()
    self:expect_kw("end")
    return params, is_vararg, body
end

function P:function_expr()
    local start = self:cur().range.start
    self:advance()                              -- 'function'
    local params, is_vararg, body = self:funcbody()
    return self:finish({ kind = "function_expr", params = params, is_vararg = is_vararg, body = body }, start)
end

-- ── statements ────────────────────────────────────────────────────────
local BLOCK_FOLLOW = { ["end"] = true, ["else"] = true, ["elseif"] = true, ["until"] = true }

function P:block_follow()
    local t = self:cur()
    return t.kind == "eof" or (t.kind == "keyword" and BLOCK_FOLLOW[t.text] == true)
end

-- block := {stat} [retstat]
function P:block()
    self.depth = self.depth + 1                         -- bound statement nesting too (shared budget)
    if self.depth > self.max_depth then
        if not self.aborted then
            self.aborted = true
            self:emit_terminal(diag.error("lua.limit.max_depth",
                "block nesting exceeds max_depth (" .. self.max_depth .. ")", self.path, self:cur().range))
        end
        self.depth = self.depth - 1
        return {}
    end
    local stmts = {}
    while not self:block_follow() and not self.aborted do
        if self:is_kw("return") then
            stmts[#stmts + 1] = self:return_stat()
            break                                       -- return is the block's last statement
        end
        local before = self.pos
        local s = self:statement()
        if s then stmts[#stmts + 1] = s end
        if self.pos == before then self:advance() end   -- stall guard: always make progress
    end
    self.depth = self.depth - 1
    return stmts
end

function P:explist()
    local list = { self:subexpr(0) }
    while self:is_op(",") do self:advance(); list[#list + 1] = self:subexpr(0) end
    return list
end

function P:statement()
    local start = self:cur().range.start
    if self:is_op(";") then self:advance(); return nil
    elseif self:is_kw("if") then return self:if_stat()
    elseif self:is_kw("while") then return self:while_stat()
    elseif self:is_kw("do") then
        self:advance(); local body = self:block(); self:expect_kw("end")
        return self:finish({ kind = "do", body = body }, start)
    elseif self:is_kw("for") then return self:for_stat()
    elseif self:is_kw("repeat") then return self:repeat_stat()
    elseif self:is_kw("function") then return self:function_decl()
    elseif self:is_kw("local") then return self:local_stat()
    elseif self:is_kw("break") then self:advance(); return self:finish({ kind = "break" }, start)
    elseif self:is_kw("goto") then
        self:advance()
        local nm = self:cur(); local label = nil
        if nm.kind == "name" then self:advance(); label = nm.text else self:err("<name> expected after 'goto'") end
        return self:finish({ kind = "goto", label = label }, start)
    elseif self:is_op("::") then return self:label_stat()
    else return self:exprstat()
    end
end

function P:label_stat()
    local start = self:cur().range.start
    self:advance()                                      -- '::'
    local nm = self:cur(); local name = nil
    if nm.kind == "name" then self:advance(); name = nm.text else self:err("<name> expected in label") end
    self:expect_op("::")
    return self:finish({ kind = "label", name = name }, start)
end

function P:return_stat()
    local start = self:cur().range.start
    self:advance()                                      -- 'return'
    local values = {}
    if not self:block_follow() and not self:is_op(";") then values = self:explist() end
    if self:is_op(";") then self:advance() end
    return self:finish({ kind = "return", values = values }, start)
end

function P:if_stat()
    local start = self:cur().range.start
    self:advance()                                      -- 'if'
    local clauses = {}
    local cond = self:subexpr(0); self:expect_kw("then")
    clauses[#clauses + 1] = { cond = cond, body = self:block() }
    while self:is_kw("elseif") do
        self:advance()
        local c = self:subexpr(0); self:expect_kw("then")
        clauses[#clauses + 1] = { cond = c, body = self:block() }
    end
    if self:is_kw("else") then
        self:advance()
        clauses[#clauses + 1] = { cond = nil, body = self:block() }   -- else clause: no cond
    end
    self:expect_kw("end")
    return self:finish({ kind = "if", clauses = clauses }, start)
end

function P:while_stat()
    local start = self:cur().range.start
    self:advance()                                      -- 'while'
    local cond = self:subexpr(0); self:expect_kw("do")
    local body = self:block(); self:expect_kw("end")
    return self:finish({ kind = "while", cond = cond, body = body }, start)
end

function P:repeat_stat()
    local start = self:cur().range.start
    self:advance()                                      -- 'repeat'
    local body = self:block(); self:expect_kw("until")
    local cond = self:subexpr(0)
    return self:finish({ kind = "repeat", body = body, cond = cond }, start)
end

function P:for_stat()
    local start = self:cur().range.start
    self:advance()                                      -- 'for'
    if self:cur().kind ~= "name" then
        self:err("<name> expected after 'for'")
        return self:finish({ kind = "error" }, start)
    end
    local n2 = self.tokens[self.pos + 1]
    if n2 and n2.kind == "op" and n2.text == "=" then   -- numeric for
        local nm = self:advance(); self:advance()       -- name, '='
        local from = self:subexpr(0); self:expect_op(",")
        local to = self:subexpr(0)
        local step = nil
        if self:is_op(",") then self:advance(); step = self:subexpr(0) end
        self:expect_kw("do"); local body = self:block(); self:expect_kw("end")
        return self:finish({ kind = "numeric_for", var = { name = nm.text, range = nm.range },
            from = from, to = to, step = step, body = body }, start)
    else                                                -- generic for
        local names = {}
        while true do
            local nm = self:cur()
            if nm.kind == "name" then self:advance(); names[#names + 1] = { name = nm.text, range = nm.range }
            else self:err("<name> expected in 'for'"); break end
            if self:is_op(",") then self:advance() else break end
        end
        self:expect_kw("in"); local exprs = self:explist()
        self:expect_kw("do"); local body = self:block(); self:expect_kw("end")
        return self:finish({ kind = "generic_for", names = names, exprs = exprs, body = body }, start)
    end
end

-- funcname := Name {'.' Name} [':' Name] -> (name-path node, is_method)
function P:funcname()
    local start = self:cur().range.start
    local nm = self:cur()
    if nm.kind ~= "name" then return self:err("<name> expected in function name"), false end
    self:advance()
    local node = self:finish({ kind = "name", name = nm.text }, start)
    while self:is_op(".") do
        self:advance()
        local f = self:cur()
        if f.kind ~= "name" then self:err("<name> expected after '.'"); break end
        self:advance(); node = self:finish({ kind = "field", obj = node, name = f.text }, start)
    end
    local is_method = false
    if self:is_op(":") then
        self:advance()
        local m = self:cur()
        if m.kind == "name" then
            self:advance(); node = self:finish({ kind = "field", obj = node, name = m.text }, start); is_method = true
        else self:err("<name> expected after ':'") end
    end
    return node, is_method
end

function P:function_decl()
    local start = self:cur().range.start
    self:advance()                                      -- 'function'
    local name, is_method = self:funcname()
    local params, is_vararg, body = self:funcbody()
    return self:finish({ kind = "function_declaration", name = name, is_local = false,
        is_method = is_method, params = params, is_vararg = is_vararg, body = body }, start)
end

function P:local_stat()
    local start = self:cur().range.start
    self:advance()                                      -- 'local'
    if self:is_kw("function") then
        self:advance()                                  -- 'function'
        local nm = self:cur(); local name = nil
        if nm.kind == "name" then self:advance(); name = self:finish({ kind = "name", name = nm.text }, nm.range.start)
        else self:err("<name> expected in 'local function'") end
        local params, is_vararg, body = self:funcbody()
        return self:finish({ kind = "function_declaration", name = name, is_local = true,
            is_method = false, params = params, is_vararg = is_vararg, body = body }, start)
    end
    local names = {}
    while true do
        local nm = self:cur()
        if nm.kind ~= "name" then self:err("<name> expected in local declaration"); break end
        self:advance()
        local attrib = nil
        if self:is_op("<") then                         -- <const> / <close> only
            self:advance()
            local a = self:cur()
            if a.kind == "name" then
                self:advance(); attrib = a.text         -- preserve the text for source fidelity
                if attrib ~= "const" and attrib ~= "close" then
                    self:err("unknown attribute '" .. attrib .. "' (expected 'const' or 'close')", a)
                end
            else
                self:err("<name> expected in attribute")
            end
            self:expect_op(">")
        end
        names[#names + 1] = { name = nm.text, attrib = attrib, range = nm.range }
        if self:is_op(",") then self:advance() else break end
    end
    local values = {}
    if self:is_op("=") then self:advance(); values = self:explist() end
    return self:finish({ kind = "local_declaration", names = names, values = values }, start)
end

-- exprstat := varlist '=' explist | functioncall
function P:exprstat()
    local start = self:cur().range.start
    local e = self:suffixed()
    if self:is_op("=") or self:is_op(",") then
        local targets = { e }
        while self:is_op(",") do self:advance(); targets[#targets + 1] = self:suffixed() end
        -- Each target must END as a name / field / index lvalue. A call inside a
        -- target is fine (f().x = 1); a call/literal/paren/etc. AS the target is
        -- not. Diagnose each invalid target but keep the node for recovery.
        for _, tg in ipairs(targets) do
            if tg.kind ~= "name" and tg.kind ~= "field" and tg.kind ~= "index" then
                self:err("cannot assign to this expression (target must be a name, field, or index)",
                    { range = tg.range })
            end
        end
        self:expect_op("=")
        local values = self:explist()
        return self:finish({ kind = "assignment", targets = targets, values = values }, start)
    elseif e.kind == "call" or e.kind == "method_call" then
        return self:finish({ kind = "call_statement", call = e }, start)
    else
        self:err("syntax error (expected assignment or function call)")
        return self:finish({ kind = "error", expr = e }, start)
    end
end

-- ── public: parse a single expression from source (slice-2 surface) ───
-- Returns (node, diagnostics). The lexer must be run by the caller (lua.lua) or
-- via M.parse_expression which does both.
function M.parse_expression(tokens, source, opts)
    opts = opts or {}
    local st = new_state(tokens, source, opts)
    local node = st:subexpr(0)
    if not st:at_eof() then
        st:err("unexpected trailing tokens after expression")
    end
    return node, st.diagnostics
end

-- ── public: parse a whole chunk (the full source AST) ────────────────
-- chunk := block. Returns (ast, diagnostics). This is what lua.parse() drives.
function M.parse_chunk(tokens, source, opts)
    opts = opts or {}
    local st = new_state(tokens, source, opts)
    local body = st:block()
    if not st:at_eof() then
        st:err("'<eof>' expected (unexpected token '" .. (st:cur().text or "") .. "')")
    end
    local ast = { kind = "chunk", body = body,
        range = { start = 1, stop = (source and (#source + 1)) or 1 } }
    return ast, st.diagnostics
end

M.new_state = new_state
return M
