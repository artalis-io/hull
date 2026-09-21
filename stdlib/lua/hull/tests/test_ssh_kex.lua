-- test_ssh_kex.lua - Tests for hull.ssh.kex
--
-- RFC 8731 (curve25519-sha256) and RFC 4253 section 7.2 (key derivation).
--
-- The orderings are what these tests are for. Both the exchange hash and the
-- KDF are bare concatenations with no internal framing, so a transposed field
-- produces key material that is wrong without anything reporting it - the
-- connection just fails to decrypt. The byte-sequence builders are pure, so
-- the exact bytes can be asserted here without a hash implementation.

local kex = require('hull.ssh.kex')
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

local Q = string.rep("\1", 32)    -- a stand-in 32-byte point

-- messages --------------------------------------------------------------

test("build_ecdh_init carries the point as a string", function()
    local p = kex.build_ecdh_init(Q)
    local r = wire.reader(p)
    assert_eq(r:byte(), kex.SSH_MSG_KEX_ECDH_INIT)
    assert_eq(r:string(), Q)
    assert_eq(r:remaining(), 0)
end)

test("build_ecdh_init demands a 32-byte point", function()
    assert_raises(function() kex.build_ecdh_init("short") end, "short")
    assert_raises(function() kex.build_ecdh_init(string.rep("x", 33)) end, "long")
end)

test("parse_ecdh_reply returns the three fields", function()
    local p = wire.writer()
        :byte(kex.SSH_MSG_KEX_ECDH_REPLY)
        :string("hostkeyblob")
        :string(Q)
        :string("signature")
        :build()
    local got = kex.parse_ecdh_reply(p)
    assert_eq(got.host_key, "hostkeyblob")
    assert_eq(got.q_s, Q)
    assert_eq(got.signature, "signature")
end)

test("parse_ecdh_reply rejects a point of the wrong length", function()
    -- X25519 accepts any 32 bytes, so a short point would otherwise be used
    -- truncated rather than refused.
    local p = wire.writer()
        :byte(kex.SSH_MSG_KEX_ECDH_REPLY)
        :string("k")
        :string(string.rep("\1", 31))
        :string("s")
        :build()
    assert_raises(function() kex.parse_ecdh_reply(p) end, "31-byte point")
end)

test("parse_ecdh_reply rejects the wrong message type", function()
    local p = wire.writer():byte(30):string("k"):string(Q):string("s"):build()
    assert_raises(function() kex.parse_ecdh_reply(p) end, "wrong type")
end)

test("parse_ecdh_reply rejects a truncated reply", function()
    local p = wire.writer():byte(kex.SSH_MSG_KEX_ECDH_REPLY):string("k"):build()
    assert_raises(function() kex.parse_ecdh_reply(p) end, "truncated")
end)

-- exchange hash ----------------------------------------------------------

local function fields()
    return {
        v_c = "SSH-2.0-Client", v_s = "SSH-2.0-Server",
        i_c = "ICICIC", i_s = "ISISIS",
        k_s = "HOSTKEY", q_c = "QQQQC", q_s = "QQQQS",
        k = "\42",
    }
end

test("exchange hash input is the RFC 8731 order", function()
    -- H = hash(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K)
    local f = fields()
    local r = wire.reader(kex.exchange_hash_input(f))
    assert_eq(r:string(), f.v_c)
    assert_eq(r:string(), f.v_s)
    assert_eq(r:string(), f.i_c)
    assert_eq(r:string(), f.i_s)
    assert_eq(r:string(), f.k_s)
    assert_eq(r:string(), f.q_c)
    assert_eq(r:string(), f.q_s)
    assert_eq(r:mpint(), f.k)
    assert_eq(r:remaining(), 0)
end)

test("the shared secret is an mpint, not a string", function()
    -- K is a number and the rest are byte strings. Encoding it as a string
    -- would hash differently from every other implementation, and the only
    -- symptom would be a connection that will not decrypt.
    local f = fields()
    f.k = "\128"                     -- high bit set: mpint must pad it
    local blob = kex.exchange_hash_input(f)
    -- the last field: 4-byte length 2, then 00 80
    assert_eq(blob:sub(-6), "\0\0\0\2\0\128")
end)

test("swapping two fields changes the bytes", function()
    -- The builder has no internal framing beyond the length prefixes, so this
    -- is the property that catches a transposition.
    local a = kex.exchange_hash_input(fields())
    local f = fields()
    f.q_c, f.q_s = f.q_s, f.q_c
    local b = kex.exchange_hash_input(f)
    assert_eq(a ~= b, true, "transposed Q_C/Q_S must not hash the same")
end)

test("exchange hash refuses a missing field", function()
    for _, name in ipairs({ "v_c", "v_s", "i_c", "i_s", "k_s", "q_c", "q_s", "k" }) do
        local f = fields()
        f[name] = nil
        assert_raises(function() kex.exchange_hash_input(f) end, "missing " .. name)
    end
end)

-- key derivation -----------------------------------------------------------

-- A stand-in hash: not cryptographic, but deterministic and dependent on
-- every input byte, which is all these structural tests need.
local function fake_hash(data)
    local acc = 0
    local out = {}
    for i = 1, #data do
        acc = (acc * 31 + data:byte(i)) % 4294967296
    end
    for i = 1, 8 do
        acc = (acc * 1103515245 + 12345) % 4294967296
        out[i] = string.char(acc % 256)
    end
    return table.concat(out)          -- 8 bytes per round
end

test("derive_key produces exactly the requested length", function()
    for _, want in ipairs({ 1, 8, 12, 16, 32, 64 }) do
        local k = kex.derive_key(fake_hash, "K", "H", "S", "A", want)
        assert_eq(#k, want, "length for " .. want)
    end
end)

test("derive_key chains when one round is not enough", function()
    -- K1 alone is 8 bytes here, so 32 bytes must come from four rounds and
    -- the first 8 must still be K1.
    local one = kex.derive_key(fake_hash, "K", "H", "S", "A", 8)
    local four = kex.derive_key(fake_hash, "K", "H", "S", "A", 32)
    assert_eq(four:sub(1, 8), one, "the first round is reused, not recomputed")
end)

test("each key id derives different material", function()
    -- A and B are the two directions; sharing material between them would let
    -- a peer replay our own ciphertext back at us.
    local seen = {}
    for _, id in pairs(kex.KEY_IDS) do
        local k = kex.derive_key(fake_hash, "K", "H", "S", id, 16)
        assert_eq(seen[k], nil, "key id " .. id .. " collided")
        seen[k] = id
    end
end)

test("derive_key rejects a bad id", function()
    assert_raises(function()
        kex.derive_key(fake_hash, "K", "H", "S", "AB", 8)
    end, "two-letter id")
    assert_raises(function()
        kex.derive_key(nil, "K", "H", "S", "A", 8)
    end, "no hash function")
end)

test("derive_keys fills the sizes for aes256-gcm", function()
    local sizes = kex.SIZES["aes256-gcm@openssh.com"]
    local keys = kex.derive_keys(fake_hash, "\1\2\3", "H", "S", sizes)
    assert_eq(#keys.iv_c2s, 12)
    assert_eq(#keys.iv_s2c, 12)
    assert_eq(#keys.key_c2s, 32)
    assert_eq(#keys.key_s2c, 32)
    -- An AEAD authenticates on its own; deriving integrity keys would imply
    -- a MAC that never gets applied.
    assert_eq(keys.mac_c2s, nil)
    assert_eq(keys.mac_s2c, nil)
end)

test("the two directions never share key material", function()
    local sizes = kex.SIZES["aes256-gcm@openssh.com"]
    local keys = kex.derive_keys(fake_hash, "\1\2\3", "H", "S", sizes)
    assert_eq(keys.key_c2s ~= keys.key_s2c, true, "encryption keys")
    assert_eq(keys.iv_c2s ~= keys.iv_s2c, true, "initial IVs")
end)

test("a different shared secret gives different keys", function()
    local sizes = kex.SIZES["aes256-gcm@openssh.com"]
    local a = kex.derive_keys(fake_hash, "\1", "H", "S", sizes)
    local b = kex.derive_keys(fake_hash, "\2", "H", "S", sizes)
    assert_eq(a.key_c2s ~= b.key_c2s, true)
end)

test("the shared secret enters the KDF as an mpint", function()
    -- derive_keys encodes K once; a caller passing the already-encoded form
    -- would double-encode, so the raw magnitude is what goes in.
    local sizes = { key_c2s = 8 }
    local viaKeys = kex.derive_keys(fake_hash, "\128", "H", "S", sizes).key_c2s
    local k_mpint = wire.writer():mpint("\128"):build()
    local direct = kex.derive_key(fake_hash, k_mpint, "H", "S", "C", 8)
    assert_eq(viaKeys, direct)
end)

-- hex bridging ---------------------------------------------------------------

test("hex round trips", function()
    local raw = "\0\1\127\128\255"
    assert_eq(kex.to_hex(raw), "00017f80ff")
    assert_eq(kex.from_hex(kex.to_hex(raw)), raw)
end)

test("from_hex rejects malformed input", function()
    assert_raises(function() kex.from_hex("abc") end, "odd length")
    assert_raises(function() kex.from_hex("zz") end, "not hex")
end)

test("raw_hash adapts a hex-returning hash", function()
    -- hull.crypto returns hex; derive_key wants raw bytes. A missed
    -- conversion here would silently halve the entropy per byte.
    local hexhash = function(d) return kex.to_hex(fake_hash(d)) end
    local raw = kex.raw_hash(hexhash)
    assert_eq(raw("x"), fake_hash("x"))
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
