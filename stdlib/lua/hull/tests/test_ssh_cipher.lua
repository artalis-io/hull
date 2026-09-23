-- test_ssh_cipher.lua - Tests for hull.ssh.cipher
--
-- The IV cases are why this file exists. Two packets under one (key, IV) in
-- GCM leak their XOR and the authentication subkey, which lets an attacker
-- forge tags for the rest of the connection. So: the counter advances exactly
-- once per packet, it is never reachable by a caller, and the two directions
-- never share one.

local cipher = require('hull.ssh.cipher')

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

local KEY = string.rep("\1", 32)
local IV  = string.rep("\0", 12)
local function zeros(n) return string.rep("\0", n) end

-- A stand-in AEAD that records what it was handed. Not cryptography: these
-- tests are about framing and IV discipline, and the real AES-GCM is pinned
-- to NIST vectors in test_crypto.c.
local function fake_aead(log)
    return {
        seal = function(key, iv, aad, plain)
            if log then log[#log + 1] = { op = "seal", iv = iv, aad = aad } end
            -- reversible, length-preserving, and dependent on the IV so a
            -- repeated IV would be visible as identical ciphertext
            local out = {}
            for i = 1, #plain do
                out[i] = string.char((plain:byte(i) + iv:byte(((i - 1) % 12) + 1)) % 256)
            end
            return table.concat(out), string.rep("\7", 16)
        end,
        open = function(key, iv, aad, ct, tag)
            if log then log[#log + 1] = { op = "open", iv = iv, aad = aad } end
            if tag ~= string.rep("\7", 16) then return nil end
            local out = {}
            for i = 1, #ct do
                out[i] = string.char((ct:byte(i) - iv:byte(((i - 1) % 12) + 1)) % 256)
            end
            return table.concat(out)
        end,
    }
end

-- construction ---------------------------------------------------------------

test("key and iv lengths are enforced", function()
    assert_raises(function() cipher.new("short", IV) end, "short key")
    assert_raises(function() cipher.new(KEY, "short") end, "short iv")
    assert_raises(function() cipher.new(nil, IV) end, "no key")
end)

test("the iv splits into a fixed part and a counter", function()
    local c = cipher.new(KEY, "\1\2\3\4" .. "\0\0\0\0\0\0\0\9")
    assert_eq(c:iv(), "\1\2\3\4\0\0\0\0\0\0\0\9")
end)

-- round trip ------------------------------------------------------------------

test("seal and open round trip", function()
    local aead = fake_aead()
    local enc = cipher.new(KEY, IV)
    local dec = cipher.new(KEY, IV)
    for _, payload in ipairs({ "", "x", string.rep("abc", 200) }) do
        local wire = enc:seal(aead, payload, zeros)
        local got, used = dec:open(aead, wire)
        assert_eq(got, payload)
        assert_eq(used, #wire)
    end
end)

test("the length field travels in the clear", function()
    -- A receiver has to know how much to read before it can decrypt anything.
    local aead = fake_aead()
    local c = cipher.new(KEY, IV)
    local wire = c:seal(aead, "hello", zeros)
    local declared = string.unpack(">I4", wire)
    assert_eq(declared, #wire - 4 - 16, "length excludes itself and the tag")
end)

test("the encrypted region is block aligned", function()
    local aead = fake_aead()
    for n = 0, 40 do
        local c = cipher.new(KEY, IV)
        local wire = c:seal(aead, string.rep("x", n), zeros)
        local declared = string.unpack(">I4", wire)
        assert_eq(declared % 16, 0, "aligned for payload " .. n)
    end
end)

test("the length is authenticated as associated data", function()
    local log = {}
    local aead = fake_aead(log)
    local c = cipher.new(KEY, IV)
    local wire = c:seal(aead, "hello", zeros)
    assert_eq(log[1].aad, wire:sub(1, 4), "AAD is the length field")
end)

-- the IV counter ---------------------------------------------------------------

test("the counter advances exactly once per packet", function()
    local log = {}
    local aead = fake_aead(log)
    local c = cipher.new(KEY, IV)
    c:seal(aead, "one", zeros)
    c:seal(aead, "two", zeros)
    c:seal(aead, "three", zeros)
    assert_eq(#log, 3)
    assert_eq(log[1].iv, "\0\0\0\0" .. "\0\0\0\0\0\0\0\0")
    assert_eq(log[2].iv, "\0\0\0\0" .. "\0\0\0\0\0\0\0\1")
    assert_eq(log[3].iv, "\0\0\0\0" .. "\0\0\0\0\0\0\0\2")
end)

test("no IV is ever repeated across many packets", function()
    -- The property that matters, stated directly.
    local log = {}
    local aead = fake_aead(log)
    local c = cipher.new(KEY, IV)
    for i = 1, 500 do c:seal(aead, "p" .. i, zeros) end
    local seen = {}
    for _, e in ipairs(log) do
        assert_eq(seen[e.iv], nil, "IV repeated")
        seen[e.iv] = true
    end
    assert_eq(c.packets, 500)
end)

test("the counter carries across a byte boundary", function()
    local log = {}
    local aead = fake_aead(log)
    local c = cipher.new(KEY, "\0\0\0\0" .. "\0\0\0\0\0\0\0\255")
    c:seal(aead, "a", zeros)
    c:seal(aead, "b", zeros)
    assert_eq(log[1].iv:sub(5), "\0\0\0\0\0\0\0\255")
    assert_eq(log[2].iv:sub(5), "\0\0\0\0\0\0\1\0", "carried into the next byte")
end)

test("the fixed part never moves", function()
    -- Even across a carry that ripples through every counter byte.
    local log = {}
    local aead = fake_aead(log)
    local c = cipher.new(KEY, "\9\8\7\6" .. "\0" .. string.rep("\255", 7))
    c:seal(aead, "a", zeros)
    c:seal(aead, "b", zeros)
    assert_eq(log[1].iv:sub(1, 4), "\9\8\7\6")
    assert_eq(log[2].iv:sub(1, 4), "\9\8\7\6", "unchanged after a full carry")
    assert_eq(log[2].iv:sub(5), "\1\0\0\0\0\0\0\0", "carry rippled correctly")
end)

test("a wrapped counter raises rather than reusing an IV", function()
    -- Unreachable in practice at 2^64 packets, but wrapping silently is the
    -- one unrecoverable bug this file could have.
    local c = cipher.new(KEY, "\0\0\0\0" .. string.rep("\255", 8))
    assert_raises(function() c:seal(fake_aead(), "x", zeros) end, "wrap")
end)

test("opening advances the counter too", function()
    -- Both ends must stay in step, or the second packet decrypts under the
    -- wrong IV.
    local aead = fake_aead()
    local enc = cipher.new(KEY, IV)
    local dec = cipher.new(KEY, IV)
    local a = enc:seal(aead, "first", zeros)
    local b = enc:seal(aead, "second", zeros)
    assert_eq(dec:open(aead, a), "first")
    assert_eq(dec:open(aead, b), "second")
end)

test("the two directions do not share a counter", function()
    local log = {}
    local aead = fake_aead(log)
    local c2s = cipher.new(KEY, IV)
    local s2c = cipher.new(KEY, IV)
    c2s:seal(aead, "a", zeros)
    s2c:seal(aead, "b", zeros)
    -- Same IV from two objects is fine ONLY because they carry different keys
    -- in practice; what matters is that one does not advance the other.
    assert_eq(c2s.packets, 1)
    assert_eq(s2c.packets, 1)
end)

-- partial reads and hostile input --------------------------------------------------

test("open asks for more while the packet is incomplete", function()
    local aead = fake_aead()
    local enc = cipher.new(KEY, IV)
    local wire = enc:seal(aead, "hello world", zeros)
    for n = 0, #wire - 1 do
        local dec = cipher.new(KEY, IV)
        local got, err = dec:open(aead, wire:sub(1, n))
        assert_eq(got, nil, "partial " .. n)
        assert_eq(err, "need_more")
        assert_eq(dec.packets, 0, "an incomplete packet must not advance the IV")
    end
end)

test("a failed tag raises and does not advance", function()
    -- A packet that does not authenticate means the stream is no longer
    -- trustworthy; resynchronising would be worse than hanging up.
    local aead = fake_aead()
    local enc = cipher.new(KEY, IV)
    local wire = enc:seal(aead, "hello", zeros)
    local tampered = wire:sub(1, #wire - 1) .. "\0"
    local dec = cipher.new(KEY, IV)
    assert_raises(function() dec:open(aead, tampered) end, "bad tag")
    assert_eq(dec.packets, 0)
end)

test("an implausible length is refused before any buffering", function()
    local dec = cipher.new(KEY, IV)
    assert_raises(function()
        dec:open(fake_aead(), "\255\255\255\255" .. string.rep("\0", 40))
    end, "huge length")
end)

test("a non-block-multiple length is refused", function()
    -- The encrypted region is whole AES blocks by construction, so anything
    -- else is a peer that is not speaking this cipher.
    local dec = cipher.new(KEY, IV)
    assert_raises(function()
        dec:open(fake_aead(), string.pack(">I4", 17) .. string.rep("\0", 60))
    end, "misaligned")
    assert_raises(function()
        dec:open(fake_aead(), string.pack(">I4", 0) .. string.rep("\0", 60))
    end, "zero length")
end)

test("padding that does not fit is refused", function()
    -- Reached only for a packet that DID authenticate, so it is a malformed
    -- peer rather than an attacker.
    local aead = {
        seal = function() return "", "" end,
        open = function() return string.char(200) .. string.rep("x", 15) end,
    }
    local dec = cipher.new(KEY, IV)
    assert_raises(function()
        dec:open(aead, string.pack(">I4", 16) .. string.rep("\0", 32))
    end, "padding exceeds packet")
end)

test("seal demands real randomness", function()
    assert_raises(function()
        cipher.new(KEY, IV):seal(fake_aead(), "x", nil)
    end, "no rng")
    assert_raises(function()
        cipher.new(KEY, IV):seal(fake_aead(), "x", function() return "short" end)
    end, "wrong length")
end)

-- the rekey limits (RFC 4253 section 9) --------------------------------------
--
-- These decide when hull.ssh.transport ASKS for new keys. Getting them wrong
-- is not visible in a handshake - it shows up as a connection that ran a
-- single key far past what it was chosen for, months later, on the one peer
-- that never rekeys by itself.

test("bytes are counted as they go on the wire, frame and tag included", function()
    -- Not the payload length: the limit is about how much an attacker has
    -- collected under one key, and that is the whole frame.
    local enc = cipher.new(KEY, IV)
    local frame = enc:seal(fake_aead(), "hello", zeros)
    assert_eq(enc:bytes_processed(), #frame, "sealed:")

    local dec = cipher.new(KEY, IV)
    dec:open(fake_aead(), frame)
    assert_eq(dec:bytes_processed(), #frame, "opened:")
end)

test("bytes accumulate across packets", function()
    local enc = cipher.new(KEY, IV)
    local total = 0
    for _ = 1, 5 do total = total + #enc:seal(fake_aead(), "abc", zeros) end
    assert_eq(enc:bytes_processed(), total)
end)

test("a fresh cipher is not due for a rekey", function()
    assert_eq(cipher.new(KEY, IV):rekey_due(), false)
end)

test("either limit on its own makes a rekey due", function()
    -- Two triggers because two things run out: the byte budget on a
    -- connection moving bulk, the invocation counter on a chatty one.
    local c = cipher.new(KEY, IV)
    c:seal(fake_aead(), "x", zeros)
    assert_eq(c:rekey_due({ bytes = 1, packets = math.huge }), true, "bytes:")
    assert_eq(c:rekey_due({ bytes = math.huge, packets = 1 }), true, "packets:")
    assert_eq(c:rekey_due({ bytes = math.huge, packets = math.huge }), false,
              "neither:")
end)

test("the default limits sit well inside the hard backstop", function()
    -- MAX_PACKETS is the thing that must never happen. If the rekey trigger
    -- ever crept up to meet it, the backstop would start firing on healthy
    -- connections and there would be no warning before it did.
    assert_eq(cipher.REKEY_PACKETS < cipher.MAX_PACKETS, true, "packets:")
    assert_eq(cipher.REKEY_BYTES > 0, true, "bytes:")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
