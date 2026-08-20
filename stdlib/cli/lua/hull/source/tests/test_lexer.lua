--
-- test_lexer.lua — slice-1 tests for hull.source.lua (lexer + ranges + line map).
--
-- Pure Lua; run by tests/hull/source/test_lua_source.c (a vanilla lua_State with
-- package.path pointed at stdlib/cli/lua). Returns { pass, fail, failures } and
-- prints FAIL lines. The Lua load() differential corpus lives in the HARNESS, not
-- here (this module never calls dynamic compilation).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lua = require("hull.source.lua")

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end
local function eq(a, b, name)
    ok(a == b, name .. " (expected " .. tostring(b) .. ", got " .. tostring(a) .. ")")
end

-- helpers over a parsed unit
local function toks(unit)  -- non-eof tokens
    local t = {}
    for _, k in ipairs(unit.tokens) do if k.kind ~= "eof" then t[#t + 1] = k end end
    return t
end
local function find(unit, text)
    for _, k in ipairs(unit.tokens) do if k.text == text then return k end end
    return nil
end

-- ── 1. exact half-open byte ranges ────────────────────────────────────
do
    local u = assert(lua.parse("local x = 42"))
    local t = toks(u)
    eq(#t, 4, "ranges: token count")
    eq(t[1].kind, "keyword", "ranges: 'local' is keyword")
    eq(t[1].range.start, 1, "ranges: local.start"); eq(t[1].range.stop, 6, "ranges: local.stop [half-open)")
    eq(t[2].text, "x", "ranges: name x"); eq(t[2].range.start, 7, "ranges: x.start"); eq(t[2].range.stop, 8, "ranges: x.stop")
    eq(find(u, "42").range.start, 11, "ranges: 42.start"); eq(find(u, "42").range.stop, 13, "ranges: 42.stop")
    -- eof is a zero-width range one past the last byte
    local eof = u.tokens[#u.tokens]
    eq(eof.kind, "eof", "ranges: last token eof"); eq(eof.range.start, 13, "ranges: eof at n+1"); eq(eof.range.stop, 13, "ranges: eof zero-width")
end

-- ── 1b. UTF-8 before a token: byte ranges must account for multibyte ──
do
    -- "-- café\n" = 2 + 1 + 3 + 2(é) + 1(\n) = 9 bytes; 'local' starts at byte 10
    local src = "-- caf\xC3\xA9\nlocal x = 1"
    local u = assert(lua.parse(src))
    eq(find(u, "local").range.start, 10, "utf8: local starts after multibyte comment")
    eq(u:text(find(u, "local")), "local", "utf8: slice round-trips")
    local sl, sc = u:position(find(u, "local").range.start)
    eq(sl, 2, "utf8: local on line 2"); eq(sc, 1, "utf8: local at col 1")
end

-- ── 2. CRLF / LF / final line without newline / shebang ───────────────
do
    local u = assert(lua.parse("local x=1\r\nlocal y=2"))   -- CRLF
    local yl = select(1, u:position(find(u, "y").range.start))
    eq(yl, 2, "crlf: y is on line 2")
    eq(u:text(find(u, "y")), "y", "crlf: slice round-trips")

    local u2 = assert(lua.parse("local x=1"))               -- no trailing newline
    eq(#u2.diagnostics, 0, "no-newline: clean")
    eq(u2.tokens[#u2.tokens].range.start, 10, "no-newline: eof at n+1")

    local u3 = assert(lua.parse("#!/usr/bin/env hull\nlocal x=1"))  -- shebang
    eq(#u3.comments, 1, "shebang: one comment"); eq(u3.comments[1].kind, "shebang", "shebang: kind")
    eq(u3.comments[1].range.start, 1, "shebang: from byte 1")
    eq(u3:text(u3.comments[1]), "#!/usr/bin/env hull\n", "shebang: lossless incl newline")
    eq(toks(u3)[1].text, "local", "shebang: first token is local")
end

-- ── 2b. lone '\r' terminates a short comment (Lua llex currIsNewline) ─
do
    -- code after a lone CR is CODE, not comment (matches real Lua 5.4)
    local u = assert(lua.parse("-- c\rx = 1"))
    eq(#u.diagnostics, 0, "cr-comment: clean")
    eq(u:text(find(u, "x")), "x", "cr-comment: 'x' after CR is a real token")
    eq(select(1, u:position(find(u, "x").range.start)), 2, "cr-comment: 'x' on line 2")
    -- a stray symbol after the CR is a syntax error (the comment did not swallow it)
    local u2 = assert(lua.parse("-- c\r@"))
    local ns = 0
    for _, d in ipairs(u2.diagnostics) do if d.code == "lua.syntax" then ns = ns + 1 end end
    ok(ns >= 1, "cr-comment: stray '@' after CR is a syntax error")
    -- CRLF still ends the comment as ONE break (no phantom empty line)
    local u3 = assert(lua.parse("-- c\r\ny = 2"))
    eq(select(1, u3:position(find(u3, "y").range.start)), 2, "crlf-comment: 'y' on line 2")
end

-- ── 3. long-bracket strings/comments at multiple = levels ─────────────
do
    local u = assert(lua.parse("local s = [==[ hi ]] there ]==]"))
    local s = find(u, "[==[ hi ]] there ]==]")
    ok(s ~= nil and s.kind == "string", "long: level-2 string, inner ]] ignored")
    eq(u:text(s), "[==[ hi ]] there ]==]", "long: lossless")
    eq(#u.diagnostics, 0, "long: no diagnostics")

    local u2 = assert(lua.parse("[[x]]"))
    eq(toks(u2)[1].text, "[[x]]", "long: level-0 string")

    local u3 = assert(lua.parse("--[=[ long comment ]=]\nlocal a=1"))
    eq(u3.comments[1].kind, "long", "long: comment kind")
    eq(u3:text(u3.comments[1]), "--[=[ long comment ]=]", "long: comment lossless")

    local u4 = assert(lua.parse("local s = [[ unterminated"))       -- unterminated long string
    ok(#u4.diagnostics >= 1, "long: unterminated -> diagnostic")
    eq(u4.diagnostics[1].code, "lua.syntax", "long: unterminated code")
end

-- ── 4. numerals + escapes (valid + malformed) ─────────────────────────
do
    for _, good in ipairs({ "42", "0xFF", "3.14", ".5", "3.", "1e10", "0x1p4", "0x1.8p3", "0xA" }) do
        local u = assert(lua.parse("local n = " .. good))
        local t = find(u, good)
        ok(t ~= nil and t.kind == "number" and not t.malformed, "number ok: " .. good)
        eq(#u.diagnostics, 0, "number ok clean: " .. good)
    end
    for _, bad in ipairs({ "0x", "1e", "1..2", "3abc" }) do
        local u = assert(lua.parse("local n = " .. bad))
        ok(#u.diagnostics >= 1, "number malformed -> diagnostic: " .. bad)
    end
    -- hex-literal exponent radix (regression, official Lua 5.4.7 math.lua): in a hex literal
    -- 'e'/'E' are DIGITS and only 'p'/'P' introduces an exponent, so `0xE+1` is `0xE + 1`
    -- (three tokens), not one malformed number. Decimal `0E+1` stays a single number.
    do
        local u = assert(lua.parse("return 0xE+1"))
        eq(#u.diagnostics, 0, "hex adj: 0xE+1 clean")
        local t = toks(u)   -- return, 0xE, +, 1
        eq(#t, 4, "hex adj: 0xE+1 -> 4 tokens")
        eq(t[2].kind, "number", "hex adj: 0xE is a number"); eq(t[2].text, "0xE", "hex adj: 0xE text")
        ok(not t[2].malformed, "hex adj: 0xE not malformed")
        eq(t[3].text, "+", "hex adj: + is a separate op")
        eq(t[4].text, "1", "hex adj: trailing 1")
        local u2 = assert(lua.parse("return 0xe-1"))
        eq(#u2.diagnostics, 0, "hex adj: 0xe-1 clean")
        eq(#toks(u2), 4, "hex adj: 0xe-1 -> 4 tokens")
        -- a hex FLOAT with a real p-exponent stays one number
        local u3 = assert(lua.parse("return 0x.ABCDEFp+24"))
        eq(#u3.diagnostics, 0, "hex adj: hex float p-exponent clean")
        eq(find(u3, "0x.ABCDEFp+24").kind, "number", "hex adj: hex float is one number")
        -- decimal exponent unaffected
        local u4 = assert(lua.parse("return 0E+1"))
        eq(#u4.diagnostics, 0, "hex adj: decimal 0E+1 clean")
        eq(find(u4, "0E+1").kind, "number", "hex adj: 0E+1 one decimal number")
    end
    -- escapes: all valid forms, no diagnostics
    local u = assert(lua.parse([[local s = "\n\t\\\"\x41\65\u{1F600}\z   x"]]))
    eq(#u.diagnostics, 0, "escapes: valid forms clean")
    -- malformed escapes
    for _, src in ipairs({ [[local s = "\x1"]], [[local s = "\999"]], [[local s = "\q"]], [[local s = "abc]] }) do
        local u2 = assert(lua.parse(src))
        ok(#u2.diagnostics >= 1, "escape/string malformed -> diagnostic: " .. src)
    end
end

-- ── 5. keyword vs identifier boundaries ───────────────────────────────
do
    local u = assert(lua.parse("and andx _end end function1 local2 nil nily"))
    local want = {
        { "and", "keyword" }, { "andx", "name" }, { "_end", "name" }, { "end", "keyword" },
        { "function1", "name" }, { "local2", "name" }, { "nil", "keyword" }, { "nily", "name" },
    }
    local t = toks(u)
    for i, w in ipairs(want) do
        eq(t[i] and t[i].text, w[1], "kw/id text " .. w[1])
        eq(t[i] and t[i].kind, w[2], "kw/id kind " .. w[1])
    end
end

-- ── 6. resource limits (bounded behavior) ─────────────────────────────
do
    local big = string.rep("x ", 100)                      -- 200 bytes
    local u = assert(lua.parse(big, { limits = { max_bytes = 10 } }))
    eq(u.diagnostics[1] and u.diagnostics[1].code, "lua.limit.max_bytes", "limit: max_bytes")

    local u2 = assert(lua.parse("a b c d e f", { limits = { max_tokens = 3 } }))
    local hit = false
    for _, d in ipairs(u2.diagnostics) do if d.code == "lua.limit.max_tokens" then hit = true end end
    ok(hit, "limit: max_tokens")

    local u3 = assert(lua.parse("@ @ @ @ @ @", { limits = { max_diagnostics = 2 } }))
    ok(#u3.diagnostics <= 2, "limit: max_diagnostics caps count (" .. #u3.diagnostics .. ")")
end

-- ── 7. lossless comment/token slicing through unit:text() ─────────────
do
    local src = table.concat({
        "#!/usr/bin/env hull",
        "-- a line comment",
        "--[[ a long comment ]]",
        'local s = "esc \\n \\x41 done"',
        "local n = 0x1.8p3",
        "local t = [==[ raw ]] block ]==]",
        "function f(a, b) return a .. b end",
    }, "\n")
    local u = assert(lua.parse(src))
    local lossless = true
    for _, k in ipairs(u.tokens) do
        if k.kind ~= "eof" and u:text(k) ~= k.text then lossless = false; print("  token slice mismatch: " .. tostring(k.text)) end
    end
    for _, cm in ipairs(u.comments) do
        if u:text(cm) ~= cm.text then lossless = false; print("  comment slice mismatch: " .. tostring(cm.text)) end
    end
    ok(lossless, "lossless: every token/comment slices back to its text")
end

-- ── 8. no raw exceptions cross parse() ────────────────────────────────
do
    local nasty = {
        "", "'", '"', "[[", "--[[", "\\", "0x", "\255\0\1", "local x = ", string.rep("(", 500),
        "[==[", "\"\\u{", "1..2..3", "::::", "...",
    }
    local all_safe = true
    for _, s in ipairs(nasty) do
        local okc, a, b = pcall(lua.parse, s)
        if not okc then all_safe = false; print("  parse RAISED on: " .. tostring(s)) end
        -- must return (unit, nil) or (nil, err): never (nil, nil)
        if okc and a == nil and b == nil then all_safe = false; print("  parse returned (nil,nil) on: " .. tostring(s)) end
    end
    ok(all_safe, "boundary: parse never raises, never returns (nil,nil)")

    -- API misuse -> (nil, err), not a raised error
    local okc, u, e = pcall(lua.parse, 123)
    ok(okc and u == nil and e ~= nil, "boundary: non-string source -> (nil, err)")
    local okc2, u2, e2 = pcall(lua.parse, nil)
    ok(okc2 and u2 == nil and e2 ~= nil, "boundary: nil source -> (nil, err)")
end

-- ── 9. escaped newline (LF / CR / CRLF / LFCR) is ONE escape sequence ─
do
    local cases = { { "\n", "LF" }, { "\r", "CR" }, { "\r\n", "CRLF" }, { "\n\r", "LFCR" } }
    for _, c in ipairs(cases) do
        local src = 'local s = "a\\' .. c[1] .. 'b"'   -- string with a backslash-escaped newline
        local u = assert(lua.parse(src))
        eq(#u.diagnostics, 0, "esc-nl " .. c[2] .. ": no diagnostic")
        local st
        for _, k in ipairs(u.tokens) do if k.kind == "string" then st = k end end
        ok(st ~= nil and not st.malformed, "esc-nl " .. c[2] .. ": one clean string token")
        eq(u:text(st), '"a\\' .. c[1] .. 'b"', "esc-nl " .. c[2] .. ": lossless range")
    end
end

-- ── 10. comments are bounded (max_comments) ───────────────────────────
do
    local many = string.rep("--c\n", 50)
    local u = assert(lua.parse(many, { limits = { max_comments = 5 } }))
    local hit = false
    for _, d in ipairs(u.diagnostics) do if d.code == "lua.limit.max_comments" then hit = true end end
    ok(hit, "limit: max_comments trips")
end

-- ── 11. terminal limit diagnostic always emits (even max_diagnostics=0) ─
do
    local u = assert(lua.parse("a b c d e", { limits = { max_tokens = 2, max_diagnostics = 0 } }))
    local hit = false
    for _, d in ipairs(u.diagnostics) do if d.code == "lua.limit.max_tokens" then hit = true end end
    ok(hit, "limit: terminal diagnostic survives max_diagnostics=0")
    local u2 = assert(lua.parse("@ @ @", { limits = { max_diagnostics = 0 } }))
    eq(#u2.diagnostics, 0, "limit: max_diagnostics=0 suppresses NORMAL diagnostics")
end

-- ── 12. opts / limits validated at the boundary (API misuse -> err) ───
do
    local function misuse(fn, name)
        local u, e = fn()
        ok(u == nil and type(e) == "string", "opts-validate: " .. name)
    end
    misuse(function() return lua.parse("x", "notatable") end, "opts non-table")
    misuse(function() return lua.parse("x", { limits = "notatable" }) end, "limits non-table")
    misuse(function() return lua.parse("x", { limits = { max_bytes = -1 } }) end, "limit negative")
    misuse(function() return lua.parse("x", { limits = { max_tokens = 1.5 } }) end, "limit non-integer")
    misuse(function() return lua.parse("x", { path = 123 }) end, "path non-string")
    local u = assert(lua.parse("x", { path = "f.lua", limits = { max_tokens = 10 } }))
    ok(u ~= nil, "opts-validate: valid opts accepted")
end

-- ── 13. unit:position clamps offsets outside [1, #source+1] ────────────
do
    local u = assert(lua.parse("ab\ncd"))                 -- #source = 5, last line = 2
    eq(select(1, u:position(1)), 1, "pos: byte 1 -> line 1")
    eq(select(2, u:position(1)), 1, "pos: byte 1 -> col 1")
    eq(select(1, u:position(0)), 1, "pos: off<1 clamps to line 1")
    eq(select(1, u:position(9999)), 2, "pos: huge off clamps to last line")
    eq(select(1, u:position(6)), 2, "pos: #source+1 is valid")
    eq(select(1, u:position("x")), 1, "pos: non-number off is safe")
end

print(string.format("test_lexer: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
