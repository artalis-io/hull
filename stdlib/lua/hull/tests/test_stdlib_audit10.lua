-- test_stdlib_audit10.lua - stdlib regressions from audit 10 (Lua).
--
-- logx / _logfmt key + control-byte escaping, cache.fetch not caching nil,
-- i18n Accept-Language order and date range / token replacement, qrcode
-- option validation, csv delimiters / quote escaping / mixed header keys /
-- BOM. The JS twin (stdlib/js/hull/tests/test_stdlib_audit10.js) asserts the
-- same outputs. Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): logx needs hull.log, cache hull.time.

local log    = require("hull.log")
local logx   = require("hull.logx")
local cache  = require("hull.cache")
local i18n   = require("hull.i18n")
local qrcode = require("hull.qrcode")
local csv    = require("hull.csv")

local pass, fail = 0, 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a), 2)
    end
end

local function raises(fn, want)
    local ok, err = pcall(fn)
    if ok then error("expected an error", 2) end
    if want and not tostring(err):find(want, 1, true) then
        error("expected error containing '" .. want .. "', got: " .. tostring(err), 2)
    end
end

-- ── logx / _logfmt ─────────────────────────────────────────────────

test("logx: keys are reduced to [A-Za-z0-9_.-]", function()
    assert_eq(logx.fields({ ["a b"] = 1 }), " a_b=1")
    assert_eq(logx.fields({ ["x=y"] = "v" }), " x_y=v")
    assert_eq(logx.fields({ ["k\nforged=1"] = "v" }), " k_forged_1=v")
    assert_eq(logx.fields({ ["\xc3\xa9"] = "v" }), " __=v")
    assert_eq(logx.fields({ [""] = "v" }), " _=v")
    assert_eq(logx.fields({ ["req.id-2_x"] = "v" }), " req.id-2_x=v")
end)

test("logx: control bytes in values are escaped and quoted", function()
    assert_eq(logx.fields({ k = "a\nb" }), ' k="a\\nb"')
    assert_eq(logx.fields({ k = "a\tb" }), ' k="a\\tb"')
    assert_eq(logx.fields({ k = "\27[31mred" }), ' k="\\x1b[31mred"')
    assert_eq(logx.fields({ k = "nul\0" }), ' k="nul\\x00"')
    assert_eq(logx.fields({ k = "del\127" }), ' k="del\\x7f"')
    assert_eq(logx.fields({ k = 'q"\\' }), ' k="q\\"\\\\"')
    assert_eq(logx.fields({ k = "plain" }), " k=plain")
end)

test("logx: a numeric key's value is logged", function()
    assert_eq(logx.fields({ [1] = "one", b = 2 }), " 1=one b=2")
end)

test("logx: the message's control bytes are escaped", function()
    local got
    local saved = log.info
    log.info = function(s) got = s end
    local ok, err = pcall(function()
        logx.with({ k = "v" }).info("line1\nforged\27x")
    end)
    log.info = saved
    assert(ok, err)
    -- Audit 11: the message is the leading msg="..." field, quoted and
    -- escaped as a value, so "k=v" inside it is not a field.
    assert_eq(got, 'msg="line1\\nforged\\x1bx" k=v')
end)

test("logx (audit 11): a message cannot forge a field", function()
    local got
    local saved = log.info
    log.info = function(s) got = s end
    local ok, err = pcall(function()
        logx.with({ k = "v" }).info('done user=admin "q')
    end)
    log.info = saved
    assert(ok, err)
    assert_eq(got, 'msg="done user=admin \\"q" k=v')
end)

test("logx (audit 11): C1 controls and U+2028 / U+2029 are escaped", function()
    assert_eq(logx.fields({ k = "a\u{85}b" }), ' k="a\\u0085b"')
    assert_eq(logx.fields({ k = "\u{80}\u{9f}" }), ' k="\\u0080\\u009f"')
    assert_eq(logx.fields({ k = "a\u{2028}b\u{2029}" }), ' k="a\\u2028b\\u2029"')
    assert_eq(logx.fields({ k = "\u{a0}" }), " k=\u{a0}", "U+00A0 is not a break")
end)

-- ── cache.fetch ────────────────────────────────────────────────────

test("cache.fetch: a nil result is not cached", function()
    local c = cache.new()
    local calls = 0
    local function f() calls = calls + 1; return nil end
    assert_eq(c.fetch("k", 60, f), nil)
    assert_eq(c.fetch("k", 60, f), nil)
    assert_eq(calls, 2, "fn runs again after a nil")
    assert_eq(c.has("k"), false)
    assert_eq(c.fetch("k", 60, function() return "v" end), "v")
    assert_eq(c.fetch("k", 60, function() return "other" end), "v", "a value is cached")
end)

-- ── i18n ───────────────────────────────────────────────────────────

test("i18n.detect: equal q keeps header order, q=0 is skipped", function()
    i18n.reset()
    i18n.load("de", { hi = "hallo" })
    i18n.load("fr", { hi = "salut" })
    i18n.load("hu", { hi = "szia" })
    assert_eq(i18n.detect("fr;q=0.5, de;q=0.5, hu;q=0.5"), "fr")
    assert_eq(i18n.detect("hu;q=0.5, de;q=0.5, fr;q=0.5"), "hu")
    assert_eq(i18n.detect("de;q=0, fr;q=0.1"), "fr", "q=0 is not acceptable")
    assert_eq(i18n.detect("de;q=0"), nil)
    assert_eq(i18n.detect("de;q=1.2.3, fr;q=0.1"), "fr", "an unparseable q is 0")
    i18n.reset()
end)

test("i18n.detect: the prefix fallback is deterministic", function()
    i18n.reset()
    i18n.load("en-US", { hi = "hi" })
    i18n.load("en-GB", { hi = "hi" })
    i18n.load("end", { hi = "x" })
    assert_eq(i18n.detect("en"), "en-GB", "sorted: en-GB before en-US")
    i18n.reset()
    i18n.load("end", { hi = "x" })
    assert_eq(i18n.detect("en"), nil, "en does not match end")
    i18n.reset()
end)

test("i18n.detect (audit 11): the base splits on '-' and '_' in both runtimes", function()
    i18n.reset()
    i18n.load("zh", { hi = "ni hao" })
    assert_eq(i18n.detect("zh_TW"), "zh", "zh_TW's base is zh")
    assert_eq(i18n.detect("zh-TW;q=0.8"), "zh")
    i18n.reset()
    i18n.load("zh_TW", { hi = "ni hao" })
    assert_eq(i18n.detect("zh_TW"), "zh_TW", "an exact zh_TW")
    assert_eq(i18n.detect("zh-HK"), "zh_TW", "prefix fallback")
    i18n.reset()
end)

test("i18n.date: out-of-range timestamps come back as text", function()
    i18n.reset()
    assert_eq(i18n.date_in(nil, 0), "1970-01-01")
    assert_eq(i18n.date_in(nil, 253402300799), "9999-12-31")
    assert_eq(i18n.date_in(nil, -62167219200), "0000-01-01")
    assert_eq(i18n.date_in(nil, -86400), "1969-12-31")
    assert_eq(i18n.date_in(nil, 1e300), tostring(1e300))
    assert_eq(i18n.date_in(nil, -1e300), tostring(-1e300))
    assert_eq(i18n.date_in(nil, 253402300800), tostring(253402300800))
end)

test("i18n.date: every occurrence of a token is replaced", function()
    i18n.reset()
    i18n.load("x", { format = { datePattern = "DD/MM/YYYY (YYYY) HH:mm:ss" } })
    i18n.load("y", { format = { datePattern = "YYYY%" } })
    -- 2001-02-03T04:05:06Z
    assert_eq(i18n.date_in("x", 981173106), "03/02/2001 (2001) 04:05:06")
    assert_eq(i18n.date_in("y", 981173106), "2001%")
    i18n.reset()
end)

-- ── qrcode ─────────────────────────────────────────────────────────

test("qrcode: scale / margin must be bounded integers", function()
    raises(function() qrcode.svg("x", { scale = 2.5 }) end, "opts.scale")
    raises(function() qrcode.svg("x", { scale = 0 }) end, "opts.scale")
    raises(function() qrcode.svg("x", { scale = 65 }) end, "opts.scale")
    raises(function() qrcode.svg("x", { scale = "4\" onload=\"x" }) end, "opts.scale")
    raises(function() qrcode.svg("x", { margin = -1 }) end, "opts.margin")
    raises(function() qrcode.svg("x", { margin = "1" }) end, "opts.margin")
    local s = qrcode.svg("x", { scale = 2, margin = 0 })
    assert(s:find('width="42"', 1, true), "21 modules * 2, no margin")
end)

test("qrcode: mask must be 0..7", function()
    raises(function() qrcode.encode("x", { mask = 8 }) end, "opts.mask")
    raises(function() qrcode.encode("x", { mask = -1 }) end, "opts.mask")
    raises(function() qrcode.encode("x", { mask = 1.5 }) end, "opts.mask")
    raises(function() qrcode.encode("x", { mask = "1" }) end, "opts.mask")
    assert_eq(qrcode.encode("x", { mask = 3 }).mask, 3)
    raises(function() qrcode.encode(42) end, "text must be a string")
end)

test("qrcode: byte mode carries the UTF-8 bytes", function()
    -- 17 two-byte characters = 34 bytes: version 3 at EC M (capacity 42),
    -- the JS side (now UTF-8 too) picks the same.
    local q = qrcode.encode(string.rep("\xc3\xa9", 17))
    assert_eq(q.version, 3)
end)

-- ── csv ────────────────────────────────────────────────────────────

test("csv: separator / quote must be one ASCII character", function()
    raises(function() csv.parse("a;;b", { separator = ";;" }) end, "opts.separator")
    raises(function() csv.encode({ { "a" } }, { separator = "" }) end, "opts.separator")
    raises(function() csv.parse("a", { quote = "''" }) end, "opts.quote")
    raises(function() csv.parse("a", { separator = "\n" }) end, "opts.separator")
    raises(function() csv.parse("a", { separator = "\xc3\xa9" }) end, "opts.separator")
    raises(function() csv.parse("a", { separator = "'", quote = "'" }) end, "must differ")
    local r = csv.parse("a\tb\n", { separator = "\t" })
    assert_eq(r[1][2], "b")
end)

test("csv: a punctuation quote is doubled literally", function()
    assert_eq(csv.encode({ { "a.b", "c" } }, { quote = "." }), ".a..b.,c\n")
    assert_eq(csv.encode({ { "50%,x" } }, { quote = "%" }), "%50%%,x%\n")
    assert_eq(csv.encode({ { 'say "hi"' } }), '"say ""hi"""\n')
end)

test("csv: header mode with mixed key types sorts deterministically", function()
    local out = csv.encode({ { [1] = "one", b = "bee", ["1"] = "s" } }, { headers = true })
    assert_eq(out, "1,1,b\none,s,bee\n")
    out = csv.encode({ { [2] = "x", a = "y", [true] = "skipped" } }, { headers = true })
    assert_eq(out, "2,a\nx,y\n")
end)

test("csv: a leading UTF-8 BOM is not data", function()
    local r = csv.parse("\xef\xbb\xbfname,age\nann,3\n", { headers = true })
    assert_eq(r[1].name, "ann")
    assert_eq(csv.parse("\xef\xbb\xbf")[1], nil)
end)

test("csv: max_rows is the exact cap", function()
    assert_eq(#csv.parse("a\nb\n", { max_rows = 2 }), 2)
    assert_eq(#csv.parse("a\nb", { max_rows = 2 }), 2)
    raises(function() csv.parse("a\nb\nc", { max_rows = 2 }) end, "max_rows")
    raises(function() csv.parse("a\nb\n\n", { max_rows = 2 }) end, "max_rows")
end)

print(string.format("stdlib audit 10: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
