--
-- hull.source.lexer — Lua 5.4 lexer (slice 1 of hull.source.lua).
--
-- Pure Lua, no native deps, no dynamic compilation (never calls load()). Produces
-- tokens and comments with EXACT half-open byte ranges { start, stop } (see
-- range.lua), plus structured diagnostics for malformed input. It NEVER raises:
-- every malformed form becomes a diagnostic and lexing continues (advancing at
-- least one byte so it always terminates). Numeral validity is checked with
-- tonumber() (a pure value conversion, not load()).
--
-- Result: { tokens = { <token>, ... }, comments = { <comment>, ... },
--           diagnostics = { <diagnostic>, ... } }
--   token   = { kind = "name"|"keyword"|"number"|"string"|"op"|"eof",
--               range = {start,stop}, text = <lexeme>, malformed = true? }
--   comment = { kind = "line"|"long"|"shebang", range = {start,stop}, text = <lexeme> }
--
-- Resource limits (opts.limits, defaults below) bound work on adversarial input:
-- max_bytes (source length), max_tokens, max_diagnostics. max_depth is a PARSER
-- limit (nesting) and is not applicable to the flat token stream.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local diag = require("hull.source.diagnostic")

local M = {}

-- Defaults bound work AND memory on adversarial build input. Every lexical item
-- (token or comment) is a small Lua table with a range subtable; millions of them
-- exhaust the tool VM even within the byte limit, so token/comment counts are
-- capped well below the byte limit's theoretical maximum.
M.DEFAULT_LIMITS = {
    max_bytes = 4 * 1024 * 1024,   -- 4 MiB of source
    max_tokens = 500000,           -- ~a 500k-token file is already pathological at build time
    max_comments = 200000,         -- comments have their own table, bounded separately
    max_diagnostics = 200,         -- caps NORMAL diagnostics; terminal limit diags always emit
    max_depth = 400,               -- parser-side; carried through for the full contract
}

local KEYWORDS = {
    ["and"]=true, ["break"]=true, ["do"]=true, ["else"]=true, ["elseif"]=true,
    ["end"]=true, ["false"]=true, ["for"]=true, ["function"]=true, ["goto"]=true,
    ["if"]=true, ["in"]=true, ["local"]=true, ["nil"]=true, ["not"]=true,
    ["or"]=true, ["repeat"]=true, ["return"]=true, ["then"]=true, ["true"]=true,
    ["until"]=true, ["while"]=true,
}

-- Multi-char operators, longest first (maximal munch).
local OPS3 = { ["..."]=true }
local OPS2 = { [".."]=true, ["::"]=true, ["=="]=true, ["~="]=true, ["<="]=true,
               [">="]=true, ["<<"]=true, [">>"]=true, ["//"]=true }
local OPS1 = "+-*/%^#&~|<>=(){}[];:,."

-- Lexer state is a small closure-captured table; helpers read/advance `pos`.
function M.tokenize(source, opts)
    opts = opts or {}
    local limits = {}
    for k, v in pairs(M.DEFAULT_LIMITS) do limits[k] = v end
    if opts.limits then for k, v in pairs(opts.limits) do limits[k] = v end end
    local path = opts.path

    local tokens, comments, diagnostics = {}, {}, {}
    local n = #source
    local pos = 1

    local function emit_diag(code, message, s, e)
        if #diagnostics >= limits.max_diagnostics then return end
        diagnostics[#diagnostics + 1] =
            diag.error(code, message, path, { start = s, stop = e })
    end

    -- Terminal (limit) diagnostics ALWAYS record, bypassing max_diagnostics, so a
    -- full diagnostic list or max_diagnostics == 0 can never silently suppress the
    -- reason lexing stopped.
    local function emit_terminal(code, message, s, e)
        diagnostics[#diagnostics + 1] =
            diag.error(code, message, path, { start = s, stop = e })
    end

    -- Source-size guard: refuse an oversized source up front (bounded work).
    if n > limits.max_bytes then
        emit_terminal("lua.limit.max_bytes",
            "source exceeds max_bytes (" .. n .. " > " .. limits.max_bytes .. ")", 1, 1)
        tokens[#tokens + 1] = { kind = "eof", range = { start = 1, stop = 1 }, text = "" }
        return { tokens = tokens, comments = comments, diagnostics = diagnostics }
    end

    local sub = string.sub
    local function at(p) return sub(source, p, p) end               -- 1-char string ("" past EOF)

    -- Opening long bracket at p: "[" ("="*) "[" -> level (# of '='), else nil.
    local function open_long(p)
        if at(p) ~= "[" then return nil end
        local q = p + 1
        while at(q) == "=" do q = q + 1 end
        if at(q) == "[" then return q - (p + 1), q + 1 end          -- level, content-start
        return nil
    end

    -- Consume a long-bracket body of `level` starting at content offset `cstart`.
    -- Returns the offset just past the closing "]"..."]" (or n+1 if unterminated).
    local function scan_long(cstart, level)
        local close = "]" .. string.rep("=", level) .. "]"
        local idx = source:find(close, cstart, true)
        if idx then return idx + #close, true end
        return n + 1, false
    end

    -- ── whitespace / newline ──────────────────────────────────────────
    local function is_space(c) return c == " " or c == "\t" or c == "\r"
        or c == "\n" or c == "\v" or c == "\f" end

    -- ── shebang: a leading '#!' line (Lua's file loader skips it). Only "#!",
    -- NOT a bare '#', so a chunk/expression beginning with the length operator
    -- (e.g. `#t`) lexes '#' as an operator rather than eating the line. ─────
    if at(1) == "#" and at(2) == "!" then
        local e = source:find("\n", 1, true)
        local stop = e and (e + 1) or (n + 1)   -- include the newline if present
        comments[#comments + 1] =
            { kind = "shebang", range = { start = 1, stop = stop }, text = sub(source, 1, stop - 1) }
        pos = stop
    end

    -- ── string-escape validation (short strings) ──────────────────────
    -- p points at '\'; returns the offset just past the escape, emitting a
    -- diagnostic for a malformed one (but always consuming >= 2 bytes).
    local function scan_escape(p)
        local e = at(p + 1)
        if e == "" then emit_diag("lua.syntax", "unfinished escape sequence", p, p + 1); return p + 1 end
        if e == "\n" or e == "\r" then                              -- \<newline>: LF, CR, CRLF, or LFCR = ONE sequence
            local q = p + 2
            local nxt = at(q)
            if (e == "\r" and nxt == "\n") or (e == "\n" and nxt == "\r") then q = q + 1 end
            return q
        end
        if e:match("[abfnrtv\\\"']") then return p + 2 end          -- simple escapes
        if e == "z" then                                            -- \z: skip following whitespace
            local q = p + 2
            while q <= n and is_space(at(q)) do q = q + 1 end
            return q
        end
        if e == "x" then
            if at(p + 2):match("%x") and at(p + 3):match("%x") then return p + 4 end
            emit_diag("lua.syntax", "\\x must be followed by two hex digits", p, math.min(p + 4, n + 1))
            return p + 2
        end
        if e:match("%d") then                                       -- \ddd (1-3 decimal, <=255)
            local q = p + 1
            local d = ""
            while #d < 3 and at(q):match("%d") do d = d .. at(q); q = q + 1 end
            if tonumber(d) > 255 then
                emit_diag("lua.syntax", "decimal escape too large (>255)", p, q)
            end
            return q
        end
        if e == "u" then                                            -- \u{XXXX}
            if at(p + 2) ~= "{" then
                emit_diag("lua.syntax", "missing '{' in \\u{xxxx}", p, p + 2); return p + 2
            end
            local q = p + 3
            local hexn = 0
            while at(q):match("%x") do q = q + 1; hexn = hexn + 1 end
            if hexn == 0 or at(q) ~= "}" then
                emit_diag("lua.syntax", "malformed \\u{xxxx} escape", p, math.min(q + 1, n + 1))
                return (at(q) == "}") and (q + 1) or q
            end
            return q + 1
        end
        emit_diag("lua.syntax", "invalid escape sequence '\\" .. e .. "'", p, p + 2)
        return p + 2
    end

    -- ── short string ("..." / '...') ──────────────────────────────────
    local function scan_short_string(start, quote)
        local p = start + 1
        while p <= n do
            local c = at(p)
            if c == quote then
                return { kind = "string", range = { start = start, stop = p + 1 },
                         text = sub(source, start, p) }
            elseif c == "\n" or c == "\r" then
                emit_diag("lua.syntax", "unfinished string (newline in short string)", start, p)
                return { kind = "string", range = { start = start, stop = p }, text = sub(source, start, p - 1), malformed = true }
            elseif c == "\\" then
                p = scan_escape(p)
            else
                p = p + 1
            end
        end
        emit_diag("lua.syntax", "unfinished string (reached end of source)", start, n + 1)
        return { kind = "string", range = { start = start, stop = n + 1 }, text = sub(source, start, n), malformed = true }
    end

    -- ── number (maximal munch, validated with tonumber) ──────────────
    local function scan_number(start)
        local p = start
        while true do
            local c = at(p)
            if c:match("[%w.]") then
                p = p + 1
            elseif (c == "+" or c == "-") and at(p - 1):match("[eEpP]") then
                p = p + 1                                            -- exponent sign
            else
                break
            end
        end
        local text = sub(source, start, p - 1)
        local malformed = tonumber(text) == nil
        if malformed then
            emit_diag("lua.syntax", "malformed number near '" .. text .. "'", start, p)
        end
        return { kind = "number", range = { start = start, stop = p }, text = text, malformed = malformed or nil }
    end

    -- ── main loop ─────────────────────────────────────────────────────
    while pos <= n do
        local c = at(pos)

        if is_space(c) then
            pos = pos + 1

        elseif c == "-" and at(pos + 1) == "-" then                 -- comment
            local start = pos
            local level, cstart = open_long(pos + 2)
            if level ~= nil then
                local stop, ok = scan_long(cstart, level)
                if not ok then emit_diag("lua.syntax", "unfinished long comment", start, n + 1) end
                comments[#comments + 1] =
                    { kind = "long", range = { start = start, stop = stop }, text = sub(source, start, stop - 1) }
                pos = stop
            else
                -- Lua ends a short comment at '\n' OR '\r' (llex.c currIsNewline), not
                -- just '\n' -- a lone '\r' terminates the comment and exposes the rest
                -- of the line as code. Stop at whichever newline byte comes first.
                local e = source:find("[\r\n]", pos)
                local stop = e or (n + 1)                           -- line comment excludes the newline
                comments[#comments + 1] =
                    { kind = "line", range = { start = start, stop = stop }, text = sub(source, start, stop - 1) }
                pos = stop
            end

        elseif c:match("[%a_]") then                                -- name or keyword
            local start = pos
            local p = pos + 1
            while at(p):match("[%w_]") do p = p + 1 end
            local text = sub(source, start, p - 1)
            tokens[#tokens + 1] = {
                kind = KEYWORDS[text] and "keyword" or "name",
                range = { start = start, stop = p }, text = text,
            }
            pos = p

        elseif c:match("%d") or (c == "." and at(pos + 1):match("%d")) then
            tokens[#tokens + 1] = scan_number(pos)
            pos = tokens[#tokens].range.stop

        elseif c == '"' or c == "'" then
            tokens[#tokens + 1] = scan_short_string(pos, c)
            pos = tokens[#tokens].range.stop

        elseif c == "[" and open_long(pos) ~= nil then              -- long string
            local start = pos
            local level, cstart = open_long(pos)
            local stop, ok = scan_long(cstart, level)
            if not ok then emit_diag("lua.syntax", "unfinished long string", start, n + 1) end
            tokens[#tokens + 1] = { kind = "string", range = { start = start, stop = stop },
                                    text = sub(source, start, stop - 1), malformed = (not ok) or nil }
            pos = stop

        else                                                        -- operator / unknown
            local three = sub(source, pos, pos + 2)
            local two = sub(source, pos, pos + 1)
            if OPS3[three] then
                tokens[#tokens + 1] = { kind = "op", range = { start = pos, stop = pos + 3 }, text = three }
                pos = pos + 3
            elseif OPS2[two] then
                tokens[#tokens + 1] = { kind = "op", range = { start = pos, stop = pos + 2 }, text = two }
                pos = pos + 2
            elseif OPS1:find(c, 1, true) then
                tokens[#tokens + 1] = { kind = "op", range = { start = pos, stop = pos + 1 }, text = c }
                pos = pos + 1
            else
                emit_diag("lua.syntax", "unexpected symbol near '" .. c .. "'", pos, pos + 1)
                pos = pos + 1                                        -- always advance -> terminates
            end
        end

        if #tokens > limits.max_tokens then
            emit_terminal("lua.limit.max_tokens",
                "token count exceeds max_tokens (" .. limits.max_tokens .. ")", pos, pos)
            break
        end
        if #comments > limits.max_comments then
            emit_terminal("lua.limit.max_comments",
                "comment count exceeds max_comments (" .. limits.max_comments .. ")", pos, pos)
            break
        end
    end

    tokens[#tokens + 1] = { kind = "eof", range = { start = n + 1, stop = n + 1 }, text = "" }
    return { tokens = tokens, comments = comments, diagnostics = diagnostics }
end

return M
