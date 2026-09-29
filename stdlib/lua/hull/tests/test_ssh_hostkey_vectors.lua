-- test_ssh_hostkey_vectors.lua - ECDSA and RSA host keys against OpenSSH/OpenSSL
--
-- The vectors are not Hull's own output. Each key was made by ssh-keygen
-- (ecdsa 256, ecdsa 384, rsa 2048); its .pub blob is `key`. The stand-in
-- exchange hash H was signed by OpenSSL with the same private key (dgst
-- -sha256 / -sha384 / -sha512 -sign), and the DER ECDSA signature rewritten
-- as SSH's mpint r, mpint s. `spki` is `openssl pkey -pubout` of the key.
--
-- So the encoding half checks Hull's DER against OpenSSL's, and the crypto
-- half checks a signature Hull did not make - the only way to know the two
-- implementations agree on what a host key signature is.

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

local H = "hull ssh host key test vector: exchange hash stand-in"
local VECTORS = {
    {
        alg = "ecdsa-sha2-nistp256",
        key = "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBL3c2BPxY64bhKyv11Wqh1J22jEDubjhu4FQmcojpNAF4TMAfedM6+xyyxtrvW9HZOllbuE3G4iuR+C4NSeqPXg=",
        sig = "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAABJAAAAIQCQymxdoon5F6gQuxLD2OHe7MUgOjrpDhiZ0G6nYLKqeAAAACBqpy1qXvgw6GPH2qUZZP27IH58EaZJbOtPTVMOogTxUQ==",
        spki = [[
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEvdzYE/FjrhuErK/XVaqHUnbaMQO5
uOG7gVCZyiOk0AXhMwB950zr7HLLG2u9b0dk6WVu4TcbiK5H4Lg1J6o9eA==
-----END PUBLIC KEY-----
]],
    },
    {
        alg = "ecdsa-sha2-nistp384",
        key = "AAAAE2VjZHNhLXNoYTItbmlzdHAzODQAAAAIbmlzdHAzODQAAABhBFThDV4Qyrw48piGnOHBu8w3gapf+zDAGTT77oNAeQg70Gz6Mo1DJaPDkhTih6ZYJ69nIcQfvbeONoKNSD1kCSPd3Ky16RFZingrp/SgC8pJsba9CyrY3VSNKQfwOdgyGg==",
        sig = "AAAAE2VjZHNhLXNoYTItbmlzdHAzODQAAABoAAAAMHzUbfOB/74kZfVz1+0appD0nJAmvqEhegdQGCnBfP2F1mfhuPTXqKWAjd1mRX/eIwAAADBT2+jU8LB2CKSW5n+P/kTwcHibH48nSS8nNmWn5SfdhQlA7vcIjE66GggJitBlxpk=",
        spki = [[
-----BEGIN PUBLIC KEY-----
MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEVOENXhDKvDjymIac4cG7zDeBql/7MMAZ
NPvug0B5CDvQbPoyjUMlo8OSFOKHplgnr2chxB+9t442go1IPWQJI93crLXpEVmK
eCun9KALykmxtr0LKtjdVI0pB/A52DIa
-----END PUBLIC KEY-----
]],
    },
    {
        alg = "rsa-sha2-256",
        key = "AAAAB3NzaC1yc2EAAAADAQABAAABAQCWnUmK7Fg7+JHycrr5gPCqW6rEBQWf7ZSEyX3heJGIpKaxBSAOyaRVtm61F6p43BmlsHxIDUEmwq/J4Ett7zyvg+FyCAVsbvxftv20BDjNVSQ+zVEg83u/KyA944MQFs98+A14JYeNI9zurhHI7ZggXY99oYriYQtiuvPLnn0mfa7s226iwFqOVXN8Tx5SxKNN8OXsajy0ZWcSWvXcuxJO7hKmA2pB9J07Qm2hWydYa8+idFklDQ05ffM3tPp8UMJOa+lz6TFC7auaeSPWV+v8U+b/2ITPeAHQ0hnW6OpxI2stTaMkWIPHGbuhvAG2OlpQAn71mykQD+EdLlkUzDDl",
        sig = "AAAADHJzYS1zaGEyLTI1NgAAAQA1fNHavkjPVzZ4JFrTIDU7sti6d7g7e7ePug+585iQXCQbfyS3qWIrVVdaHFqmqOIvegupVs0GYp7xAo8q31oHr47WuZg+FLiu0OICtUlLgaa6pXegLAwMYaz65tGt6qxfSJz9Eiuse2RLnDh8OuTbpWxnI2yBPI++X1wN7uce17Gwone++46lLCKFYXftDz7aC49XiL6vsx5YHgZACREMzeEVChRAb6/ikbgX0KSw8bOctQyuSYnfk4sXoqC7uiVTfc1CfSQhff3zoV21fWwzuSjjeY7ZpNLyhIVm5VUECDDsvJBkZtVIxBGjcqo3A6ROOzFMPlRXgaOyCMjBn0P7",
        spki = [[
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAlp1JiuxYO/iR8nK6+YDw
qluqxAUFn+2UhMl94XiRiKSmsQUgDsmkVbZutReqeNwZpbB8SA1BJsKvyeBLbe88
r4PhcggFbG78X7b9tAQ4zVUkPs1RIPN7vysgPeODEBbPfPgNeCWHjSPc7q4RyO2Y
IF2PfaGK4mELYrrzy559Jn2u7NtuosBajlVzfE8eUsSjTfDl7Go8tGVnElr13LsS
Tu4SpgNqQfSdO0JtoVsnWGvPonRZJQ0NOX3zN7T6fFDCTmvpc+kxQu2rmnkj1lfr
/FPm/9iEz3gB0NIZ1ujqcSNrLU2jJFiDxxm7obwBtjpaUAJ+9ZspEA/hHS5ZFMww
5QIDAQAB
-----END PUBLIC KEY-----
]],
    },
    {
        alg = "rsa-sha2-512",
        key = "AAAAB3NzaC1yc2EAAAADAQABAAABAQCWnUmK7Fg7+JHycrr5gPCqW6rEBQWf7ZSEyX3heJGIpKaxBSAOyaRVtm61F6p43BmlsHxIDUEmwq/J4Ett7zyvg+FyCAVsbvxftv20BDjNVSQ+zVEg83u/KyA944MQFs98+A14JYeNI9zurhHI7ZggXY99oYriYQtiuvPLnn0mfa7s226iwFqOVXN8Tx5SxKNN8OXsajy0ZWcSWvXcuxJO7hKmA2pB9J07Qm2hWydYa8+idFklDQ05ffM3tPp8UMJOa+lz6TFC7auaeSPWV+v8U+b/2ITPeAHQ0hnW6OpxI2stTaMkWIPHGbuhvAG2OlpQAn71mykQD+EdLlkUzDDl",
        sig = "AAAADHJzYS1zaGEyLTUxMgAAAQBlvST03KDNBAG1taeds/7+jJdAx+2CK4DhsHxgZfWuQEnBjDybLSs0KK7wzAlAZAO+FBqqycqUflU1/P2DYV8VBDDVOpmJB51Hv4nsl8W1PNXCsfosm3oEXxJd26bHlNK+8k3vZgOaLtqRMFH/tABu22hr9WmQPvrGs9BXdnGypTZY9OLuUjje+h5BmxAnic9evq4JWOC9MvAkcaBHA6Gacop1vgSxu8G7WUl5UAamxHwNix+bgcD2uZm+/HlCtX0Hc93tdffEciWuRAoeBBGQk5lEKdnsYmeGYa5YH0XrxLAGA+IesbxTtMmeenzn/CG/VEZ4Jm8LcJ2yUIUyHqS2",
        spki = [[
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAlp1JiuxYO/iR8nK6+YDw
qluqxAUFn+2UhMl94XiRiKSmsQUgDsmkVbZutReqeNwZpbB8SA1BJsKvyeBLbe88
r4PhcggFbG78X7b9tAQ4zVUkPs1RIPN7vysgPeODEBbPfPgNeCWHjSPc7q4RyO2Y
IF2PfaGK4mELYrrzy559Jn2u7NtuosBajlVzfE8eUsSjTfDl7Go8tGVnElr13LsS
Tu4SpgNqQfSdO0JtoVsnWGvPonRZJQ0NOX3zN7T6fFDCTmvpc+kxQu2rmnkj1lfr
/FPm/9iEz3gB0NIZ1ujqcSNrLU2jJFiDxxm7obwBtjpaUAJ+9ZspEA/hHS5ZFMww
5QIDAQAB
-----END PUBLIC KEY-----
]],
    },
}

local base64 = require('hull.encoding').base64

-- Real hull.crypto when this runs inside Hull (the lua_ssh_bridge leg in
-- tests/hull/runtime/lua/test_lua.c); absent in the vanilla harness, where
-- only the encoding half runs. The C leg asserts the full count, so the
-- verify half cannot be skipped there without failing.
local has_crypto, crypto = pcall(require, "hull.crypto")
if not (has_crypto and type(crypto) == "table" and type(crypto.verify) == "function") then
    crypto = nil
end

local function vec(alg)
    for _, v in ipairs(VECTORS) do
        if v.alg == alg then return v end
    end
    error("no vector " .. alg)
end
local function blob(v) return base64.decode(v.key) end
local function sigb(v) return base64.decode(v.sig) end

-- parsing and the PEM bridge -------------------------------------------------

test("OpenSSH ECDSA and RSA key blobs parse", function()
    local k = hostkey.parse_key(blob(vec("ecdsa-sha2-nistp256")))
    assert_eq(k.type, "ecdsa-sha2-nistp256")
    assert_eq(#k.q, 65)
    k = hostkey.parse_key(blob(vec("ecdsa-sha2-nistp384")))
    assert_eq(#k.q, 97)
    k = hostkey.parse_key(blob(vec("rsa-sha2-256")))
    assert_eq(k.type, "ssh-rsa")
    assert_eq(k.bits, 2048)
    assert_eq(k.e, "\1\0\1")
end)

test("the PEM built from each blob is byte-for-byte OpenSSL's", function()
    -- The expected text is `openssl pkey -pubout` of the same key. Byte
    -- equality means the DER (lengths, the leading zero on the modulus, the
    -- curve OIDs) is right, not merely accepted by a lenient parser.
    for _, v in ipairs(VECTORS) do
        assert_eq(hostkey.public_key_pem(hostkey.parse_key(blob(v))), v.spki, v.alg)
    end
end)

test("an ECDSA signature becomes fixed-width r || s", function()
    -- The P-256 vector's r has a leading zero byte in its mpint (33 bytes):
    -- it must come out as exactly 32.
    local s = hostkey.parse_signature(sigb(vec("ecdsa-sha2-nistp256")))
    assert_eq(#s.signature, 64)
    s = hostkey.parse_signature(sigb(vec("ecdsa-sha2-nistp384")))
    assert_eq(#s.signature, 96)
end)

test("verify hands crypto.verify the right algorithm, key and data", function()
    local want = { ["ecdsa-sha2-nistp256"] = "ES256", ["ecdsa-sha2-nistp384"] = "ES384",
                   ["rsa-sha2-256"] = "RS256", ["rsa-sha2-512"] = "RS512" }
    for _, v in ipairs(VECTORS) do
        local seen = {}
        local stub = { verify = function(jose, pem, data, sig)
            seen.jose, seen.pem, seen.data, seen.len = jose, pem, data, #sig
            return true
        end }
        assert_eq(hostkey.verify_signature(stub, blob(v), sigb(v), H, v.alg), true, v.alg)
        assert_eq(seen.jose, want[v.alg], v.alg)
        assert_eq(seen.pem, v.spki, v.alg)
        assert_eq(seen.data, H, v.alg)
        if v.alg:find("rsa", 1, true) then assert_eq(seen.len, 256, v.alg) end
    end
end)

-- the negotiated algorithm decides ---------------------------------------------

local function always() return { verify = function() return true end } end

test("a signature under another algorithm than the one negotiated is refused", function()
    -- A server that negotiated SHA-512 and then signs with SHA-256 is refused
    -- even though that signature is genuine.
    local v = vec("rsa-sha2-256")
    local ok, why = hostkey.verify_signature(always(), blob(v), sigb(v), H, "rsa-sha2-512")
    assert_eq(ok, false)
    assert_eq(why:find("negotiated", 1, true) ~= nil, true, why)
end)

test("a key cannot sign under an algorithm for another key type", function()
    local k = blob(vec("ecdsa-sha2-nistp256"))
    local s = sigb(vec("rsa-sha2-256"))
    local ok, why = hostkey.verify_signature(always(), k, s, H, "rsa-sha2-256")
    assert_eq(ok, false)
    assert_eq(why:find("cannot sign", 1, true) ~= nil, true, why)
end)

test("SHA-1 ssh-rsa signatures are not accepted", function()
    local v = vec("rsa-sha2-256")
    local inner = wire.reader(sigb(v)); inner:string()
    local sha1 = wire.writer():string("ssh-rsa"):string(inner:string()):build()
    local ok = hostkey.verify_signature(always(), blob(v), sha1, H)
    assert_eq(ok, false)
end)

-- keys that are refused -------------------------------------------------------

local function rsa_blob(e, n)
    return wire.writer():string("ssh-rsa"):mpint(e):mpint(n):build()
end

test("an RSA key below 2048 bits is refused", function()
    local n1024 = "\192" .. string.rep("\1", 127)
    local ok, err = pcall(hostkey.parse_key, rsa_blob("\1\0\1", n1024))
    assert_eq(ok, false)
    assert_eq(tostring(err):find("minimum", 1, true) ~= nil, true, tostring(err))
end)

test("an RSA key with an even or unit exponent is refused", function()
    local n = "\192" .. string.rep("\1", 255)
    assert_raises(function() hostkey.parse_key(rsa_blob("\1\0\0", n)) end, "even e")
    assert_raises(function() hostkey.parse_key(rsa_blob("\1", n)) end, "e = 1")
end)

test("a compressed or wrong-curve ECDSA point is refused", function()
    local q = "\2" .. string.rep("\1", 32)
    assert_raises(function()
        hostkey.parse_key(wire.writer():string("ecdsa-sha2-nistp256"):string("nistp256")
                              :string(q):build())
    end, "compressed")
    assert_raises(function()
        hostkey.parse_key(wire.writer():string("ecdsa-sha2-nistp256"):string("nistp384")
                              :string("\4" .. string.rep("\1", 64)):build())
    end, "curve mismatch")
end)

test("a key blob with trailing bytes is refused", function()
    assert_raises(function() hostkey.parse_key(blob(vec("rsa-sha2-256")) .. "\0") end)
end)

test("an RSA signature longer than the modulus is refused", function()
    local v = vec("rsa-sha2-256")
    local long = wire.writer():string("rsa-sha2-256"):string(string.rep("\1", 257)):build()
    local ok, why = hostkey.verify_signature(always(), blob(v), long, H, "rsa-sha2-256")
    assert_eq(ok, false)
    assert_eq(why:find("longer than the modulus", 1, true) ~= nil, true, why)
end)

test("a short RSA signature is padded to the modulus length", function()
    -- RFC 8332 section 3; some servers drop the leading zero bytes.
    local v = vec("rsa-sha2-256")
    local seen
    local stub = { verify = function(_, _, _, sig) seen = sig; return true end }
    local short = wire.writer():string("rsa-sha2-256"):string(string.rep("\7", 250)):build()
    hostkey.verify_signature(stub, blob(v), short, H, "rsa-sha2-256")
    assert_eq(#seen, 256)
    assert_eq(seen:sub(1, 6), string.rep("\0", 6))
end)

-- against the real hull.crypto --------------------------------------------------

if crypto then
    test("the OpenSSL signatures verify through hull.crypto", function()
        for _, v in ipairs(VECTORS) do
            local ok, why = hostkey.verify_signature(crypto, blob(v), sigb(v), H, v.alg)
            assert_eq(ok, true, v.alg .. " " .. tostring(why))
        end
    end)

    test("a different exchange hash does not verify", function()
        for _, v in ipairs(VECTORS) do
            local ok = hostkey.verify_signature(crypto, blob(v), sigb(v), H .. "!", v.alg)
            assert_eq(ok, false, v.alg)
        end
    end)

    test("a signature with one byte changed does not verify", function()
        for _, v in ipairs(VECTORS) do
            local s = sigb(v)
            local i = #s - 3
            local bad = s:sub(1, i - 1) .. string.char(s:byte(i) ~ 1) .. s:sub(i + 1)
            local ok = hostkey.verify_signature(crypto, blob(v), bad, H, v.alg)
            assert_eq(ok, false, v.alg)
        end
    end)
end

return { pass = pass, fail = fail, real_crypto = crypto ~= nil }
