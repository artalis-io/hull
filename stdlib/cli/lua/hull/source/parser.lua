--
-- hull.source.parser — recursive-descent Lua 5.4 parser (slice 2: expressions).
--
-- Consumes the lexer token stream and builds Hull AST expression nodes with EXACT
-- half-open byte ranges (see range.lua). Statements/declarations land in slice 3;
-- a function EXPRESSION's body is a statement block, so in slice 2 the body is
-- skipped (balanced to its `end`) and a `lua.unsupported` diagnostic is emitted
-- (a non-empty body is valid syntax this slice does not yet represent -- an
-- explicit diagnostic, never a silently malformed AST). Slice 3 replaces the skip
-- with the real block parser.
--
-- The parser NEVER raises: a parse error emits a diagnostic and returns an
-- { kind = "error" } node (best-effort). Node vocabulary (expressions) matches
-- docs/lua_source_analysis_design.md §5.
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

function M.parse_expression_state(st)
    return st:subexpr(0)
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

-- ── function expression (params now; body skipped until slice 3) ──────
-- Balanced skip to the matching `end`. The `end`-terminated openers are
-- `function`, `if`, `for`, `while`, and a STANDALONE `do`. A `for`/`while` opens
-- ONE block whose `do` is a syntactic marker, not a second `end`-taker -- so its
-- `do` is absorbed (tracked by `pending_do`, a count, which stays correct even
-- when a nested function literal appears inside a loop header). `repeat ... until`
-- is NOT `end`-terminated and is ignored for the `end` count. Replaced by the real
-- block parser in slice 3.
function P:skip_block_to_end()
    local open = 1              -- the function's own `end`
    local pending_do = 0        -- for/while opened but awaiting their `do`
    while not self:at_eof() do
        local t = self:cur()
        if t.kind == "keyword" then
            local k = t.text
            if k == "function" or k == "if" then
                open = open + 1
            elseif k == "for" or k == "while" then
                open = open + 1; pending_do = pending_do + 1
            elseif k == "do" then
                if pending_do > 0 then pending_do = pending_do - 1 else open = open + 1 end
            elseif k == "end" then
                open = open - 1
                if open == 0 then return self:advance() end
            end
            -- repeat / until are not end-terminated: ignored for `open`.
        end
        self:advance()
    end
    self:err("'end' expected (unterminated function)")
    return nil
end

function P:function_expr()
    local start = self:cur().range.start
    self:advance()                              -- 'function'
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
    -- body: slice 3. Empty body (immediate `end`) is fine; a non-empty one is
    -- valid syntax not yet represented -> explicit diagnostic + balanced skip.
    local body_start = self:cur().range.start
    if not self:is_kw("end") then
        self:skip_block_to_end()
        self:unsupported("function body is parsed in a later slice", body_start,
            (self.prev and self.prev.range.stop) or body_start)
    else
        self:advance()                          -- 'end'
    end
    return self:finish({ kind = "function_expr", params = params, is_vararg = is_vararg, body = nil }, start)
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

M.new_state = new_state
return M
