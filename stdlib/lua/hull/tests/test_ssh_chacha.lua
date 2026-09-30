-- test_ssh_chacha.lua - Tests for hull.ssh.chacha
--
-- The chacha20-poly1305@openssh.com packet layer. The framing (which key
-- encrypts what, where the tag goes, what the sequence number binds) is
-- checked with stand-in primitives, so the vanilla harness can run it; the
-- caps-bearing leg in test_lua.c runs the same file against hull.crypto's
-- real ChaCha20 and Poly1305 and adds a round trip through them. Interop
-- with OpenSSH itself is the live sshd e2e (tests/e2e_ssh_tunnel.sh).

local chacha = require('hull.ssh.chacha')
local packet = require('hull.ssh.packet')

local pass, fail = 0, 0
local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name .. ": " .. tostring(err)) end
end
local function assert_eq(a, b, msg)
    if a ~= b then error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a), 2) end
end
local function assert_raises(fn, want)
    local ok, err = pcall(fn)
    if ok then error("should have raised", 2) end
    if want and not tostring(err):find(want, 1, true) then
        error("raised " .. tostring(err) .. ", wanted " .. want, 2)
    end
end

-- Stand-ins with the right shapes and the one property the framing relies on:
-- output depends on key, nonce and counter. Not cryptography.
local function toy_stream(key, nonce, counter, data)
    local seed = 0
    for i = 1, #key do seed = (seed * 31 + key:byte(i)) % 2147483647 end
    for i = 1, #nonce do seed = (seed * 31 + nonce:byte(i)) % 2147483647 end
    local out, x = {}, (seed + counter * 64) % 2147483647
    for i = 1, #data do
        x = (x * 48271 + i) % 2147483647
        out[i] = string.char(data:byte(i) ~ (x & 0xFF))
    end
    return table.concat(out)
end
local function toy_mac(key, msg)
    local h = 0
    for i = 1, #key do h = (h * 131 + key:byte(i)) % 4294967291 end
    for i = 1, #msg do h = (h * 131 + msg:byte(i)) % 4294967291 end
    return string.pack(">I8I8", h, h ~ 0x5555)
end
local TOY = { chacha20 = toy_stream, poly1305 = toy_mac,
              ct_eq = function(a, b) return a == b end }

local has_crypto, crypto = pcall(require, "hull.crypto")
has_crypto = has_crypto and type(crypto.chacha20) == "function"
local REAL = has_crypto and {
    chacha20 = crypto.chacha20, poly1305 = crypto.poly1305, ct_eq = crypto.constant_time_eq,
} or nil

local KEY = string.rep("\1", 32) .. string.rep("\2", 32)
local function zeros(n) return string.rep("\0", n) end

for _, prims in ipairs({ TOY, REAL }) do
    local tag = prims == TOY and "[toy] " or "[real] "

    test(tag .. "a sealed packet opens under the same key and sequence number", function()
        local a, b = chacha.new(KEY, prims), chacha.new(KEY, prims)
        local frame = a:seal(nil, "hello, server", zeros, 7)
        assert_eq(b:needed(frame, 7), #frame)
        local payload, used = b:open(nil, frame, 7)
        assert_eq(payload, "hello, server")
        assert_eq(used, #frame)
    end)

    test(tag .. "the length is encrypted, so the frame cannot be sized by peeking", function()
        local frame = chacha.new(KEY, prims):seal(nil, "x", zeros, 0)
        local plain_len = #frame - chacha.LENGTH_LEN - chacha.TAG_LEN
        assert_eq(string.unpack(">I4", frame) ~= plain_len, true)
    end)

    test(tag .. "another sequence number does not open it", function()
        local frame = chacha.new(KEY, prims):seal(nil, "payload", zeros, 3)
        assert_raises(function() chacha.new(KEY, prims):open(nil, frame, 4) end)
    end)

    test(tag .. "a flipped byte anywhere fails authentication", function()
        local frame = chacha.new(KEY, prims):seal(nil, "payload payload", zeros, 1)
        -- In the body (the length would be caught as implausible or as a
        -- different size; the tag covers both).
        local i = chacha.LENGTH_LEN + 3
        local bad = frame:sub(1, i - 1) .. string.char(frame:byte(i) ~ 1) .. frame:sub(i + 1)
        assert_raises(function() chacha.new(KEY, prims):open(nil, bad, 1) end,
                      "failed authentication")
        local t = #frame
        local badtag = frame:sub(1, t - 1) .. string.char(frame:byte(t) ~ 1)
        assert_raises(function() chacha.new(KEY, prims):open(nil, badtag, 1) end,
                      "failed authentication")
    end)

    test(tag .. "the payload key and the length key are different halves", function()
        -- Swapping the halves must not open: each key has one job.
        local swapped = KEY:sub(33, 64) .. KEY:sub(1, 32)
        local frame = chacha.new(KEY, prims):seal(nil, "payload", zeros, 0)
        assert_raises(function() chacha.new(swapped, prims):open(nil, frame, 0) end)
    end)

    test(tag .. "an incomplete frame asks for more rather than failing", function()
        local frame = chacha.new(KEY, prims):seal(nil, "payload", zeros, 9)
        local r = chacha.new(KEY, prims)
        assert_eq(select(2, r:open(nil, frame:sub(1, 3), 9)), "need_more")
        assert_eq(select(2, r:open(nil, frame:sub(1, #frame - 1), 9)), "need_more")
        assert_eq(r:needed(frame:sub(1, 3), 9), 4)
    end)

    test(tag .. "padding aligns the encrypted region to 8, length excluded", function()
        for n = 0, 20 do
            local frame = chacha.new(KEY, prims):seal(nil, string.rep("p", n), zeros, 0)
            local region = #frame - chacha.LENGTH_LEN - chacha.TAG_LEN
            assert_eq(region % 8, 0, "payload " .. n .. ":")
            assert_eq(region - 1 - n >= packet.MIN_PADDING, true)
        end
    end)
end

test("an implausible decrypted length is refused before any wait", function()
    -- Forge a frame whose length decrypts (under the toy stream) to 2^31.
    local c = chacha.new(KEY, TOY)
    local nonce = "\0\0\0\0" .. string.pack(">I8", 5)
    local huge = toy_stream(KEY:sub(33, 64), nonce, 0, string.pack(">I4", 0x80000000))
    assert_raises(function() c:needed(huge .. zeros(40), 5) end, "exceeds the maximum")
    local odd = toy_stream(KEY:sub(33, 64), nonce, 0, string.pack(">I4", 13))
    assert_raises(function() c:needed(odd .. zeros(40), 5) end, "whole number of blocks")
end)

test("a key of the wrong size, or missing primitives, is refused", function()
    assert_raises(function() chacha.new(string.rep("k", 32), TOY) end, "64 bytes")
    assert_raises(function() chacha.new(KEY, { chacha20 = toy_stream }) end)
end)

test("a sequence number is required", function()
    assert_raises(function() chacha.new(KEY, TOY):seal(nil, "x", zeros, nil) end,
                  "sequence number")
end)

return { pass = pass, fail = fail }
