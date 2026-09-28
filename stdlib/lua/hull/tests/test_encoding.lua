-- test_encoding.lua - Tests for hull.encoding
--
-- RFC 4648 vectors for every codec, and the strict/lenient decoding rules.
-- stdlib/js/hull/tests/test_encoding.js asserts exactly the same values, so
-- the two runtimes cannot drift apart silently.

local enc = require('hull.encoding')
local hex, base64, base32 = enc.hex, enc.base64, enc.base32

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(got, want, msg)
    if got ~= want then
        error((msg or "") .. " expected " .. tostring(want) .. ", got " .. tostring(got), 2)
    end
end

local function assert_raises(fn, msg)
    if pcall(fn) then error((msg or "should have raised") .. " but did not", 2) end
end

local ALL = {}
for i = 0, 255 do ALL[#ALL + 1] = string.char(i) end
ALL = table.concat(ALL)

-- The RFC 4648 section 10 inputs.
local IN = { "", "f", "fo", "foo", "foob", "fooba", "foobar" }

-- hex -------------------------------------------------------------------------

test("hex: RFC 4648 vectors, lowercase", function()
    local want = { "", "66", "666f", "666f6f", "666f6f62", "666f6f6261", "666f6f626172" }
    for i, s in ipairs(IN) do assert_eq(hex.encode(s), want[i], s) end
    assert_eq(hex.encode("\0\1\127\128\255"), "00017f80ff")
end)

test("hex: every byte round trips, and either case decodes", function()
    assert_eq(hex.decode(hex.encode(ALL)), ALL)
    assert_eq(hex.decode("00017F80FF"), "\0\1\127\128\255")
end)

test("hex: strict decoding", function()
    assert_eq(select(2, hex.decode("abc")), "bad_length")
    assert_eq(select(2, hex.decode("zz")), "invalid_char")
    assert_eq(select(2, hex.decode(" f")), "invalid_char")
    assert_eq(select(2, hex.decode("-f")), "invalid_char")
    assert_eq(hex.decode(""), "")
    assert_raises(function() hex.decode(nil) end, "non-string")
    assert_raises(function() hex.encode(42) end, "non-string")
end)

-- base64 ----------------------------------------------------------------------

test("base64: RFC 4648 vectors, padded and not", function()
    local padded = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" }
    local bare   = { "", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy" }
    for i, s in ipairs(IN) do
        assert_eq(base64.encode(s), padded[i], s)
        assert_eq(base64.encode(s, { pad = false }), bare[i], s)
        assert_eq(base64.decode(padded[i]), s, "decode padded " .. s)
        assert_eq(base64.decode(bare[i]), s, "decode bare " .. s)
    end
end)

test("base64: url alphabet, unpadded by default", function()
    assert_eq(base64.encode("\251\255\191", { url = true }), "-_-_")
    assert_eq(base64.encode("\251\255\191"), "+/+/")
    assert_eq(base64.encode("f", { url = true }), "Zg")
    assert_eq(base64.encode("f", { url = true, pad = true }), "Zg==")
    assert_eq(base64.decode("-_-_", { url = true }), "\251\255\191")
end)

test("base64: every byte round trips in both alphabets", function()
    assert_eq(base64.decode(base64.encode(ALL)), ALL)
    assert_eq(base64.decode(base64.encode(ALL, { url = true }), { url = true }), ALL)
end)

test("base64: strict decoding", function()
    assert_eq(select(2, base64.decode("Zm9v!YmFy")), "invalid_char")
    assert_eq(select(2, base64.decode("Zm9v-YmFy")), "invalid_char", "url char, std alphabet")
    assert_eq(select(2, base64.decode("+/+/", { url = true })), "invalid_char", "std char, url alphabet")
    assert_eq(select(2, base64.decode("Zg==", { url = true })), "bad_padding", "url takes no padding")
    assert_eq(select(2, base64.decode("Zm9v\nYmFy")), "invalid_char", "whitespace needs lenient")
    assert_eq(select(2, base64.decode("Zg=")), "bad_padding")
    assert_eq(select(2, base64.decode("Zm9v=")), "bad_padding")
    assert_eq(select(2, base64.decode("Zg===")), "bad_padding")
    assert_eq(select(2, base64.decode("Zg==Zg==")), "bad_padding", "data after padding")
    assert_eq(select(2, base64.decode("Zm9vY")), "bad_length")
    -- One encoding per value: the unused low bits must be zero.
    assert_eq(select(2, base64.decode("Zh==")), "non_canonical")
    assert_eq(select(2, base64.decode("Zm9=")), "non_canonical")
    assert_eq(select(2, base64.decode("Zh", { url = true })), "non_canonical")
end)

test("base64: lenient decoding skips whitespace only", function()
    assert_eq(base64.decode("Zm9v\nYmFy\r\n", { lenient = true }), "foobar")
    assert_eq(base64.decode("Zm9v YmE=\n", { lenient = true }), "fooba")
    assert_eq(select(2, base64.decode("Zm9v!YmFy", { lenient = true })), "invalid_char")
end)

-- base32 ----------------------------------------------------------------------

test("base32: RFC 4648 vectors, unpadded", function()
    local want = { "", "MY", "MZXQ", "MZXW6", "MZXW6YQ", "MZXW6YTB", "MZXW6YTBOI" }
    for i, s in ipairs(IN) do
        assert_eq(base32.encode(s), want[i], s)
        assert_eq(base32.decode(want[i]), s, "decode " .. s)
        assert_eq(base32.decode(want[i]:lower()), s, "lowercase " .. s)
    end
end)

test("base32: every byte round trips", function()
    assert_eq(base32.decode(base32.encode(ALL)), ALL)
end)

test("base32: strict, and lenient for pasted secrets", function()
    assert_eq(select(2, base32.decode("MZXW6===")), "invalid_char")
    assert_eq(select(2, base32.decode("MZXW 6YTB")), "invalid_char")
    assert_eq(select(2, base32.decode("MZXW1")), "invalid_char")
    assert_eq(base32.decode("mzxw 6ytb oi==\n", { lenient = true }), "foobar")
    assert_eq(select(2, base32.decode("MZXW1", { lenient = true })), "invalid_char")
    -- Lengths nothing encodes to (1, 3, 6 mod 8), and non-zero unused bits.
    assert_eq(select(2, base32.decode("M")), "bad_length")
    assert_eq(select(2, base32.decode("MZX")), "bad_length")
    assert_eq(select(2, base32.decode("MZXW6Y")), "bad_length")
    assert_eq(select(2, base32.decode("MZ")), "non_canonical")
end)

-- utf8 ------------------------------------------------------------------------

-- The byte forms the JS suite produces from "héllo", "€", U+1F600.
local TEXT = { "h\xc3\xa9llo", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "" }

test("utf8: well-formed text round trips", function()
    for _, t in ipairs(TEXT) do
        assert_eq(enc.utf8.encode(t), t)
        assert_eq(enc.utf8.decode(t), t)
    end
end)

test("utf8: malformed bytes do not decode", function()
    local bad = {
        "\xc0\x80",          -- overlong NUL
        "\xed\xa0\x80",      -- a surrogate
        "\xf4\x90\x80\x80",  -- above U+10FFFF
        "\xe2\x82",          -- truncated
        "\x80",              -- a bare continuation byte
        "ok\xff",            -- never valid
    }
    for _, b in ipairs(bad) do
        assert_eq(select(2, enc.utf8.decode(b)), "invalid_utf8", enc.hex.encode(b))
    end
    assert_raises(function() enc.utf8.decode(nil) end, "non-string")
end)

-- url -------------------------------------------------------------------------
-- The same vectors as the JS suite; the text there is the UTF-8 here.

test("url: RFC 3986 unreserved stays, everything else is %XX upper case", function()
    local url = enc.url
    assert_eq(url.encode(""), "")
    assert_eq(url.encode("AZaz09-._~"), "AZaz09-._~")
    assert_eq(url.encode("a b/c?d=e&f"), "a%20b%2Fc%3Fd%3De%26f")
    assert_eq(url.encode("!'()*+"), "%21%27%28%29%2A%2B")
    assert_eq(url.encode("h\xc3\xa9"), "h%C3%A9")
    assert_eq(url.encode("\0\xff"), "%00%FF")
end)

test("url: keep leaves further ASCII characters alone", function()
    local url = enc.url
    assert_eq(url.encode("a!#$&+^`|b c", { keep = "!#$&+^`|" }), "a!#$&+^`|b%20c")
    assert_raises(function() url.encode("x", { keep = "\xc3\xa9" }) end, "non-ASCII keep")
end)

test("url: decode reads escapes in either case, and '+' only in form mode", function()
    local url = enc.url
    assert_eq(url.decode("a%20b%2fc%2F"), "a b/c/")
    assert_eq(url.decode("a+b"), "a+b")
    assert_eq(url.decode("a+b%2B", { form = true }), "a b+")
    assert_eq(url.decode("h%C3%A9"), "h\xc3\xa9")
    assert_eq(url.decode("plain"), "plain")
end)

test("url: a malformed escape leaves the whole value as it is", function()
    local url = enc.url
    assert_eq(url.decode("a%2"), "a%2")
    assert_eq(url.decode("%zz%41"), "%zz%41")
    assert_eq(url.decode("100%"), "100%")
    assert_eq(url.decode("x+%g1", { form = true }), "x %g1")
end)

test("url: every byte round trips", function()
    assert_eq(enc.url.decode(enc.url.encode(ALL)), ALL)
    assert_raises(function() enc.url.encode(nil) end, "non-string")
    assert_raises(function() enc.url.decode(42) end, "non-string")
end)

return { pass = pass, fail = fail }
