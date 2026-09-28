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
end)

return { pass = pass, fail = fail }
