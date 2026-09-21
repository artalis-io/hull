-- test_ssh_hostkey.lua - Tests for hull.ssh.hostkey
--
-- This is the module that decides whether Hull is talking to the machine it
-- meant to. The trust-state cases below are the point: an unknown host and a
-- CHANGED host must never be conflated, because the second one is what a
-- man-in-the-middle looks like.

local hostkey = require('hull.ssh.hostkey')
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

local function to_hex(raw)
    return (raw:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

local KEY_A = string.rep("\1", 32)
local KEY_B = string.rep("\2", 32)

local function key_blob(key)
    return wire.writer():string("ssh-ed25519"):string(key):build()
end

local function sig_blob(sig)
    return wire.writer():string("ssh-ed25519"):string(sig):build()
end

-- an in-memory store, the shape an application supplies
local function new_store(seed)
    local t = {}
    for k, v in pairs(seed or {}) do t[k] = v end
    return {
        get = function(host) return t[host] end,
        put = function(host, blob) t[host] = blob end,
        forget = function(host) t[host] = nil end,
        _table = t,
    }
end

-- blob parsing -------------------------------------------------------------

test("parse_key reads an ssh-ed25519 blob", function()
    local k = hostkey.parse_key(key_blob(KEY_A))
    assert_eq(k.algorithm, "ssh-ed25519")
    assert_eq(k.key, KEY_A)
end)

test("parse_key refuses another algorithm", function()
    -- Only Ed25519 is negotiated, so anything else means the server sent a
    -- key for an algorithm we never agreed to.
    local blob = wire.writer():string("ssh-rsa"):string(KEY_A):build()
    assert_raises(function() hostkey.parse_key(blob) end, "ssh-rsa")
end)

test("parse_key refuses a key of the wrong length", function()
    assert_raises(function()
        hostkey.parse_key(key_blob(string.rep("\1", 31)))
    end, "31 bytes")
    assert_raises(function()
        hostkey.parse_key(key_blob(string.rep("\1", 33)))
    end, "33 bytes")
end)

test("parse_key refuses a truncated blob", function()
    assert_raises(function()
        hostkey.parse_key(key_blob(KEY_A):sub(1, 10))
    end, "truncated")
end)

test("parse_signature reads and length-checks", function()
    local s = hostkey.parse_signature(sig_blob(string.rep("\9", 64)))
    assert_eq(s.signature, string.rep("\9", 64))
    assert_raises(function()
        hostkey.parse_signature(sig_blob(string.rep("\9", 63)))
    end, "63-byte sig")
end)

-- base64 and fingerprints ------------------------------------------------------

test("base64 matches the RFC 4648 vectors", function()
    -- A fingerprint is read aloud and compared against ssh-keygen output, so
    -- "nearly base64" would be worse than useless.
    local cases = {
        { "", "" }, { "f", "Zg" }, { "fo", "Zm8" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg" }, { "fooba", "Zm9vYmE" }, { "foobar", "Zm9vYmFy" },
    }
    for _, c in ipairs(cases) do
        assert_eq(hostkey.base64_nopad(c[1]), c[2], "base64 of " .. c[1])
    end
end)

test("base64 emits no padding", function()
    for n = 1, 12 do
        local out = hostkey.base64_nopad(string.rep("x", n))
        assert_eq(out:find("=", 1, true), nil, "padding at n=" .. n)
    end
end)

test("base64 handles bytes above 127", function()
    -- Digest bytes are not ASCII; a signed-byte bug would show up here.
    assert_eq(hostkey.base64_nopad("\255\254\253"), "//79")
    assert_eq(hostkey.base64_nopad("\0\0\0"), "AAAA")
end)

test("fingerprint has the OpenSSH shape", function()
    local fake_sha = function() return string.rep("\0", 32) end
    local fp = hostkey.fingerprint(fake_sha, key_blob(KEY_A))
    assert_eq(fp:sub(1, 7), "SHA256:")
    -- 32 bytes of digest is 43 base64 characters with no padding.
    assert_eq(#fp, 7 + 43)
    assert_eq(fp, "SHA256:" .. string.rep("A", 43))
end)

test("fingerprint needs a hash function", function()
    assert_raises(function()
        hostkey.fingerprint(nil, key_blob(KEY_A))
    end, "no hash")
end)

-- trust states ------------------------------------------------------------------

test("an unseen host is unknown, not trusted", function()
    local store = new_store()
    local status, stored = hostkey.check(store, "spark.local", key_blob(KEY_A))
    assert_eq(status, hostkey.UNKNOWN)
    assert_eq(stored, nil)
end)

test("a matching stored key is trusted", function()
    local store = new_store({ ["spark.local"] = key_blob(KEY_A) })
    local status = hostkey.check(store, "spark.local", key_blob(KEY_A))
    assert_eq(status, hostkey.TRUSTED)
end)

test("a different key for a known host is CHANGED, never unknown", function()
    -- Conflating this with "unknown" is what turns a man-in-the-middle into a
    -- successful one.
    local store = new_store({ ["spark.local"] = key_blob(KEY_A) })
    local status, stored = hostkey.check(store, "spark.local", key_blob(KEY_B))
    assert_eq(status, hostkey.CHANGED)
    assert_eq(stored, key_blob(KEY_A))
end)

test("trust is per host, not global", function()
    local store = new_store({ ["a.local"] = key_blob(KEY_A) })
    assert_eq(hostkey.check(store, "b.local", key_blob(KEY_A)), hostkey.UNKNOWN)
end)

test("check demands a real store", function()
    assert_raises(function() hostkey.check(nil, "h", key_blob(KEY_A)) end, "nil store")
    assert_raises(function() hostkey.check({}, "h", key_blob(KEY_A)) end, "no get")
end)

-- accepting and forgetting -------------------------------------------------------

test("accept_new remembers a first-use key", function()
    local store = new_store()
    hostkey.accept_new(store, "spark.local", key_blob(KEY_A))
    assert_eq(hostkey.check(store, "spark.local", key_blob(KEY_A)), hostkey.TRUSTED)
end)

test("accept_new refuses to overwrite", function()
    -- Trust on first use means FIRST use. Replacing a key has to be a
    -- deliberate two-step act, not a side effect of accepting one.
    local store = new_store({ ["spark.local"] = key_blob(KEY_A) })
    assert_raises(function()
        hostkey.accept_new(store, "spark.local", key_blob(KEY_B))
    end, "overwrite")
    -- and the original is untouched
    assert_eq(store.get("spark.local"), key_blob(KEY_A))
end)

test("forget then accept_new is how a rotated host is re-trusted", function()
    local store = new_store({ ["spark.local"] = key_blob(KEY_A) })
    hostkey.forget(store, "spark.local")
    assert_eq(hostkey.check(store, "spark.local", key_blob(KEY_B)), hostkey.UNKNOWN)
    hostkey.accept_new(store, "spark.local", key_blob(KEY_B))
    assert_eq(hostkey.check(store, "spark.local", key_blob(KEY_B)), hostkey.TRUSTED)
end)

-- signature verification -----------------------------------------------------------

local function crypto_stub(result)
    return { ed25519_verify = function() return result end }
end

test("verify_signature passes a good signature through", function()
    local ok = hostkey.verify_signature(crypto_stub(true), to_hex,
        key_blob(KEY_A), sig_blob(string.rep("\9", 64)), "HASH")
    assert_eq(ok, true)
end)

test("verify_signature reports a bad signature", function()
    local ok, err = hostkey.verify_signature(crypto_stub(false), to_hex,
        key_blob(KEY_A), sig_blob(string.rep("\9", 64)), "HASH")
    assert_eq(ok, false)
    assert_eq(err:find("does not verify", 1, true) ~= nil, true, err)
end)

test("a malformed blob is a verification failure, not a crash", function()
    local ok, err = hostkey.verify_signature(crypto_stub(true), to_hex,
        "garbage", sig_blob(string.rep("\9", 64)), "HASH")
    assert_eq(ok, false)
    assert_eq(type(err), "string")
end)

test("the signature is checked over the exchange hash", function()
    -- Binding the key to THIS exchange is what stops a replayed signature
    -- from a different session.
    local seen
    local crypto = { ed25519_verify = function(data) seen = data; return true end }
    hostkey.verify_signature(crypto, to_hex, key_blob(KEY_A),
        sig_blob(string.rep("\9", 64)), "THE-EXCHANGE-HASH")
    assert_eq(seen, "THE-EXCHANGE-HASH")
end)

-- the whole decision -------------------------------------------------------------

local function fake_sha(data) return (data .. string.rep("\0", 32)):sub(1, 32) end

test("verify refuses before it ever looks at the store", function()
    -- A bad signature means the peer does not hold the key it presented, so
    -- whether we have seen that key before is beside the point.
    local store = new_store({ ["spark.local"] = key_blob(KEY_A) })
    local d = hostkey.verify(crypto_stub(false), to_hex, fake_sha, store,
        "spark.local", key_blob(KEY_A), sig_blob(string.rep("\9", 64)), "H")
    assert_eq(d.ok, false)
    assert_eq(d.status, nil)
end)

test("verify reports trusted for a known matching host", function()
    local store = new_store({ ["spark.local"] = key_blob(KEY_A) })
    local d = hostkey.verify(crypto_stub(true), to_hex, fake_sha, store,
        "spark.local", key_blob(KEY_A), sig_blob(string.rep("\9", 64)), "H")
    assert_eq(d.ok, true)
    assert_eq(d.status, hostkey.TRUSTED)
    assert_eq(d.fingerprint:sub(1, 7), "SHA256:")
end)

test("verify exposes the fingerprint for an unknown host", function()
    -- The key is shown BEFORE trust, so an operator can be asked rather than
    -- told afterwards.
    local store = new_store()
    local d = hostkey.verify(crypto_stub(true), to_hex, fake_sha, store,
        "spark.local", key_blob(KEY_A), sig_blob(string.rep("\9", 64)), "H")
    assert_eq(d.status, hostkey.UNKNOWN)
    assert_eq(d.fingerprint ~= nil, true)
    assert_eq(d.key_blob, key_blob(KEY_A))
    -- and nothing was written: remembering is the caller's explicit act
    assert_eq(store.get("spark.local"), nil)
end)

test("verify never returns trusted for a changed key", function()
    local store = new_store({ ["spark.local"] = key_blob(KEY_A) })
    local d = hostkey.verify(crypto_stub(true), to_hex, fake_sha, store,
        "spark.local", key_blob(KEY_B), sig_blob(string.rep("\9", 64)), "H")
    assert_eq(d.status, hostkey.CHANGED)
    assert_eq(d.status ~= hostkey.TRUSTED, true)
end)

test("a changed key reports both fingerprints", function()
    -- The useful question for whoever reads this is which key they expected.
    local store = new_store({ ["spark.local"] = key_blob(KEY_A) })
    local d = hostkey.verify(crypto_stub(true), to_hex, fake_sha, store,
        "spark.local", key_blob(KEY_B), sig_blob(string.rep("\9", 64)), "H")
    assert_eq(d.stored_fingerprint ~= nil, true)
    assert_eq(d.fingerprint ~= d.stored_fingerprint, true)
end)

test("a valid signature does not by itself mean trusted", function()
    -- The signature proves the peer holds the key it showed. It says nothing
    -- about whether that is the key we expected, which is the whole reason a
    -- trust store exists.
    local store = new_store()
    local d = hostkey.verify(crypto_stub(true), to_hex, fake_sha, store,
        "new.local", key_blob(KEY_A), sig_blob(string.rep("\9", 64)), "H")
    assert_eq(d.ok, true)
    assert_eq(d.status, hostkey.UNKNOWN)
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
