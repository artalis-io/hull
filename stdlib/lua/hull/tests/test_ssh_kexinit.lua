-- test_ssh_kexinit.lua - Tests for hull.ssh.kexinit
--
-- RFC 4253 section 7.1. The negotiation cases matter most: this is where a
-- server gets to influence which algorithms Hull will then use.

local kexinit = require('hull.ssh.kexinit')
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

local COOKIE = string.rep("\7", 16)

-- a server KEXINIT that agrees with everything Hull offers
local function agreeable()
    return {
        cookie = COOKIE,
        kex = { "curve25519-sha256" },
        host_key = { "ssh-ed25519" },
        cipher_c2s = { "aes256-gcm@openssh.com" },
        cipher_s2c = { "aes256-gcm@openssh.com" },
        mac_c2s = { "hmac-sha2-256" },
        mac_s2c = { "hmac-sha2-256" },
        compression_c2s = { "none" },
        compression_s2c = { "none" },
        languages_c2s = {}, languages_s2c = {},
        first_kex_packet_follows = false, reserved = 0,
    }
end

-- build and parse --------------------------------------------------------

test("build produces a parseable KEXINIT", function()
    local p = kexinit.build(nil, COOKIE)
    assert_eq(string.byte(p, 1), kexinit.SSH_MSG_KEXINIT)
    local got = kexinit.parse(p)
    assert_eq(got.cookie, COOKIE)
    assert_eq(got.kex[1], "curve25519-sha256")
    assert_eq(got.host_key[1], "ssh-ed25519")
    assert_eq(got.cipher_c2s[1], "aes256-gcm@openssh.com")
    assert_eq(got.compression_c2s[1], "none")
    assert_eq(got.first_kex_packet_follows, false)
    assert_eq(got.reserved, 0)
end)

test("build demands a 16-byte cookie", function()
    assert_raises(function() kexinit.build(nil, "short") end, "short cookie")
    assert_raises(function() kexinit.build(nil, nil) end, "no cookie")
    assert_raises(function()
        kexinit.build(nil, string.rep("x", 17))
    end, "long cookie")
end)

test("Hull never sets first_kex_packet_follows", function()
    -- Guessing costs a round trip and discard bookkeeping for no gain to a
    -- tool that opens one connection per host.
    assert_eq(kexinit.parse(kexinit.build(nil, COOKIE)).first_kex_packet_follows,
              false)
end)

test("parse rejects a different message type", function()
    local p = string.char(21) .. string.rep("\0", 40)
    assert_raises(function() kexinit.parse(p) end, "wrong msg type")
end)

test("parse rejects a truncated message", function()
    local p = kexinit.build(nil, COOKIE)
    assert_raises(function() kexinit.parse(p:sub(1, 20)) end, "truncated")
end)

test("parse tolerates trailing bytes", function()
    -- RFC 4253 reserves room for extension; an unknown trailing field must
    -- not make us hang up on a newer server.
    local p = kexinit.build(nil, COOKIE) .. "future extension"
    local got = kexinit.parse(p)
    assert_eq(got.kex[1], "curve25519-sha256")
end)

test("compression is offered as none only", function()
    -- Compressing attacker-influenced plaintext before encrypting it is how
    -- CRIME worked.
    local got = kexinit.parse(kexinit.build(nil, COOKIE))
    assert_eq(#got.compression_c2s, 1)
    assert_eq(got.compression_c2s[1], "none")
end)

-- choose ------------------------------------------------------------------

test("choose honours the client order, not the server order", function()
    -- RFC 4253 section 7.1: the client preference decides.
    local got = kexinit.choose({ "a", "b" }, { "b", "a" })
    assert_eq(got, "a")
end)

test("choose returns nil when there is no overlap", function()
    assert_eq(kexinit.choose({ "a" }, { "b" }), nil)
    assert_eq(kexinit.choose({}, { "a" }), nil)
    assert_eq(kexinit.choose({ "a" }, {}), nil)
end)

-- negotiate ----------------------------------------------------------------

test("negotiate agrees with a cooperative server", function()
    local n, err = kexinit.negotiate(nil, agreeable())
    assert_eq(err, nil)
    assert_eq(n.kex, "curve25519-sha256")
    assert_eq(n.host_key, "ssh-ed25519")
    assert_eq(n.cipher_c2s, "aes256-gcm@openssh.com")
    assert_eq(n.cipher_s2c, "aes256-gcm@openssh.com")
    assert_eq(n.compression_c2s, "none")
end)

test("negotiate accepts the libssh name for the same exchange", function()
    local s = agreeable()
    s.kex = { "curve25519-sha256@libssh.org" }
    local n = kexinit.negotiate(nil, s)
    assert_eq(n.kex, "curve25519-sha256@libssh.org")
end)

test("negotiate refuses a server with no common cipher", function()
    local s = agreeable()
    s.cipher_s2c = { "3des-cbc", "aes128-cbc" }
    local n, err = kexinit.negotiate(nil, s)
    assert_eq(n, nil)
    -- The message has to name the category and both sides, or an operator
    -- cannot tell which knob to turn.
    assert_eq(err:find("server-to-client cipher", 1, true) ~= nil, true, err)
    assert_eq(err:find("3des-cbc", 1, true) ~= nil, true, err)
end)

test("negotiate refuses a server offering only weak kex", function()
    local s = agreeable()
    s.kex = { "diffie-hellman-group1-sha1" }
    local n, err = kexinit.negotiate(nil, s)
    assert_eq(n, nil)
    assert_eq(err:find("key exchange", 1, true) ~= nil, true, err)
end)

test("negotiate refuses a server offering only RSA host keys", function()
    local s = agreeable()
    s.host_key = { "ssh-rsa", "rsa-sha2-512" }
    local n, err = kexinit.negotiate(nil, s)
    assert_eq(n, nil)
    assert_eq(err:find("host key", 1, true) ~= nil, true, err)
end)

test("negotiate refuses compression", function()
    local s = agreeable()
    s.compression_c2s = { "zlib@openssh.com" }
    s.compression_s2c = { "zlib@openssh.com" }
    local n, err = kexinit.negotiate(nil, s)
    assert_eq(n, nil)
    assert_eq(err:find("compression", 1, true) ~= nil, true, err)
end)

test("a missing MAC is fine under an AEAD cipher", function()
    -- Authentication comes from the cipher, so an empty MAC list is not a
    -- reason to refuse the connection.
    local s = agreeable()
    s.mac_c2s = {}
    s.mac_s2c = {}
    local n, err = kexinit.negotiate(nil, s)
    assert_eq(err, nil)
    assert_eq(n.mac_c2s, nil)
    assert_eq(n.cipher_c2s, "aes256-gcm@openssh.com")
end)

test("an empty server offer is refused, not defaulted", function()
    local s = agreeable()
    s.kex = {}
    local n = kexinit.negotiate(nil, s)
    assert_eq(n, nil)
end)

-- guessed packets ------------------------------------------------------------

test("a guess that matches is not discarded", function()
    local s = agreeable()
    s.first_kex_packet_follows = true
    local n = kexinit.negotiate(nil, s)
    assert_eq(kexinit.guess_was_wrong(s, n), false)
end)

test("a guess on a different kex is discarded", function()
    -- RFC 4253 section 7.1: the guess is wrong unless BOTH the kex and the
    -- host key algorithm match what was negotiated.
    local s = agreeable()
    s.first_kex_packet_follows = true
    s.kex = { "diffie-hellman-group14-sha256", "curve25519-sha256" }
    local n = kexinit.negotiate(nil, s)
    assert_eq(n.kex, "curve25519-sha256")
    assert_eq(kexinit.guess_was_wrong(s, n), true)
end)

test("no guess means nothing to discard", function()
    local s = agreeable()
    local n = kexinit.negotiate(nil, s)
    assert_eq(kexinit.guess_was_wrong(s, n), false)
end)

-- the offer itself -------------------------------------------------------------

test("the default offer lists only algorithms Hull can do", function()
    local o = kexinit.DEFAULT_OFFER
    -- Every name here is one a peer may steer us onto.
    assert_eq(#o.cipher, 1)
    assert_eq(o.cipher[1], "aes256-gcm@openssh.com")
    assert_eq(#o.host_key, 1)
    assert_eq(o.host_key[1], "ssh-ed25519")
    assert_eq(#o.compression, 1)
    assert_eq(o.compression[1], "none")
    for _, name in ipairs(o.kex) do
        assert_eq(name:find("curve25519", 1, true) ~= nil, true, name)
    end
end)

test("the built message round trips through the wire codec", function()
    local p = kexinit.build(nil, COOKIE)
    local r = wire.reader(p)
    assert_eq(r:byte(), 20)
    assert_eq(r:raw(16), COOKIE)
    assert_eq(r:namelist()[1], "curve25519-sha256")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
