-- test_ssh_wire.lua - Tests for hull.ssh.wire
--
-- The SSH binary data types (RFC 4251 section 5). These bytes arrive from an
-- unauthenticated peer, so the truncation and malformed-input cases matter as
-- much as the round trips.

local wire = require('hull.ssh.wire')

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

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

local function assert_raises(fn, msg)
    local ok = pcall(fn)
    if ok then error((msg or "should have raised") .. " but did not") end
end

local function hex(s)
    return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

-- round trips --------------------------------------------------------

test("byte round trip", function()
    local b = wire.writer():byte(0):byte(127):byte(255):build()
    assert_eq(#b, 3)
    local r = wire.reader(b)
    assert_eq(r:byte(), 0)
    assert_eq(r:byte(), 127)
    assert_eq(r:byte(), 255)
    assert_eq(r:remaining(), 0)
end)

test("uint32 is big endian", function()
    local b = wire.writer():uint32(0x01020304):build()
    assert_eq(hex(b), "01020304")
    assert_eq(wire.reader(b):uint32(), 0x01020304)
end)

test("uint32 boundary values", function()
    for _, v in ipairs({ 0, 1, 0xFFFFFFFF }) do
        local b = wire.writer():uint32(v):build()
        assert_eq(wire.reader(b):uint32(), v)
    end
end)

test("uint64 round trip", function()
    local b = wire.writer():uint64(0):uint64(4294967296):build()
    local r = wire.reader(b)
    assert_eq(r:uint64(), 0)
    assert_eq(r:uint64(), 4294967296)
end)

test("string is length prefixed and binary safe", function()
    local payload = "a\0b\255c"
    local b = wire.writer():string(payload):build()
    assert_eq(hex(b:sub(1, 4)), "00000005")
    assert_eq(wire.reader(b):string(), payload)
end)

test("empty string round trip", function()
    local b = wire.writer():string(""):build()
    assert_eq(hex(b), "00000000")
    assert_eq(wire.reader(b):string(), "")
end)

test("boolean round trip", function()
    local b = wire.writer():boolean(true):boolean(false):build()
    assert_eq(hex(b), "0100")
    local r = wire.reader(b)
    assert_eq(r:boolean(), true)
    assert_eq(r:boolean(), false)
end)

test("any non-zero byte reads as true", function()
    assert_eq(wire.reader("\2"):boolean(), true)
    assert_eq(wire.reader("\255"):boolean(), true)
end)

-- mpint ---------------------------------------------------------------

test("mpint zero is an empty string", function()
    local b = wire.writer():mpint(""):build()
    assert_eq(hex(b), "00000000")
    assert_eq(wire.reader(b):mpint(), "")
    assert_eq(hex(wire.writer():mpint("\0\0"):build()), "00000000")
end)

test("mpint pads a high bit so it stays positive", function()
    local b = wire.writer():mpint("\128"):build()
    assert_eq(hex(b), "000000020080")
    assert_eq(hex(wire.reader(b):mpint()), "80")
end)

test("mpint does not pad when the high bit is clear", function()
    local b = wire.writer():mpint("\127"):build()
    assert_eq(hex(b), "000000017f")
end)

test("mpint strips unnecessary leading zeros", function()
    local b = wire.writer():mpint("\0\0\1\2"):build()
    assert_eq(hex(b), "000000020102")
end)

test("mpint reader rejects a negative value", function()
    local b = "\0\0\0\1\128"
    assert_raises(function() wire.reader(b):mpint() end, "negative mpint")
end)

-- name lists ----------------------------------------------------------

test("namelist round trip", function()
    local b = wire.writer():namelist({ "ssh-ed25519", "rsa-sha2-256" }):build()
    assert_eq(wire.reader(b):string(), "ssh-ed25519,rsa-sha2-256")
    local got = wire.reader(b):namelist()
    assert_eq(#got, 2)
    assert_eq(got[1], "ssh-ed25519")
    assert_eq(got[2], "rsa-sha2-256")
end)

test("empty namelist is legal", function()
    local b = wire.writer():namelist({}):build()
    assert_eq(hex(b), "00000000")
    assert_eq(#wire.reader(b):namelist(), 0)
end)

test("namelist refuses a comma inside a name", function()
    assert_raises(function()
        wire.writer():namelist({ "a,b" })
    end, "comma in name")
end)

test("namelist reader rejects an empty name", function()
    for _, s in ipairs({ "a,,b", ",a", "a," }) do
        assert_raises(function()
            wire.reader(wire.string(s)):namelist()
        end, "empty name in " .. s)
    end
end)

-- truncation ----------------------------------------------------------

test("every reader raises on truncation", function()
    assert_raises(function() wire.reader(""):byte() end, "byte")
    assert_raises(function() wire.reader("\1\2"):uint32() end, "uint32")
    assert_raises(function() wire.reader("\1\2\3\4"):uint64() end, "uint64")
    assert_raises(function() wire.reader("\0\0\0\10abc"):string() end, "string")
    assert_raises(function() wire.reader("\0\0\0\10ab"):mpint() end, "mpint")
    assert_raises(function() wire.reader("ab"):raw(5) end, "raw")
end)

test("a huge claimed length does not allocate", function()
    local b = "\255\255\255\255" .. "short"
    assert_raises(function() wire.reader(b):string() end, "huge length")
end)

test("reader tracks its own position", function()
    local b = wire.writer():byte(1):string("hi"):byte(2):build()
    local r = wire.reader(b)
    assert_eq(r:remaining(), #b)
    assert_eq(r:byte(), 1)
    assert_eq(r:string(), "hi")
    assert_eq(r:byte(), 2)
    assert_eq(r:remaining(), 0)
end)

-- writer validation ----------------------------------------------------

test("writer rejects out-of-range values", function()
    assert_raises(function() wire.writer():byte(256) end, "byte 256")
    assert_raises(function() wire.writer():byte(-1) end, "byte -1")
    assert_raises(function() wire.writer():uint32(-1) end, "uint32 -1")
    assert_raises(function() wire.writer():uint32(0x100000000) end, "uint32 big")
    assert_raises(function() wire.writer():uint64(-1) end, "uint64 -1")
    assert_raises(function() wire.writer():string(42) end, "string of number")
end)

test("a built packet composes in order", function()
    local b = wire.writer()
        :byte(20)
        :string("cookie")
        :namelist({ "curve25519-sha256" })
        :boolean(false)
        :uint32(0)
        :build()
    local r = wire.reader(b)
    assert_eq(r:byte(), 20)
    assert_eq(r:string(), "cookie")
    assert_eq(r:namelist()[1], "curve25519-sha256")
    assert_eq(r:boolean(), false)
    assert_eq(r:uint32(), 0)
    assert_eq(r:remaining(), 0)
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
