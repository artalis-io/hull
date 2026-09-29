-- test_ssh_ecdsa_key.lua - ECDSA user keys, against keys ssh-keygen wrote
--
-- EC256 / EC384 / EC521 are `ssh-keygen -t ecdsa -b 256|384|521`; EC256_ENC
-- is EC256's sibling protected with `ssh-keygen -p` (bcrypt + aes256-ctr).
-- *_SEC1 is the same key after `ssh-keygen -m PEM`, which OpenSSL writes as
-- a SEC1 "EC PRIVATE KEY": the PEM Hull builds from the OpenSSH file must be
-- byte-for-byte that. *_SPKI is `ssh-keygen -e -m PKCS8`, the public key a
-- signature is checked against. ECDSA signatures are randomised, so the
-- crypto half verifies them rather than comparing bytes.
--
-- The vanilla harness runs the structural half with a stand-in crypto; the
-- caps-bearing leg in test_lua.c runs it all with the real hull.crypto and
-- requires the full count.

local privatekey = require('hull.ssh.privatekey')
local userauth = require('hull.ssh.userauth')
local wire = require('hull.ssh.wire')
local base64 = require('hull.encoding').base64

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

local EC256 = [[
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAaAAAABNlY2RzYS
1zaGEyLW5pc3RwMjU2AAAACG5pc3RwMjU2AAAAQQTXIJbvJD1ZRwlCd7dVtumHVA2EbNn+
8P8qucTjO9wnkXR2g2442W4IDqZkk5pNyYOKydgz586rRMhUYF3abIADAAAAqBpKohkaSq
IZAAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBNcglu8kPVlHCUJ3
t1W26YdUDYRs2f7w/yq5xOM73CeRdHaDbjjZbggOpmSTmk3Jg4rJ2DPnzqtEyFRgXdpsgA
MAAAAgVMhUSTzSgrpOsVsqWOgxu60xiq5wGl2xhrvXgHyUjmoAAAAKaHVsbC1lYzI1NgEC
AwQFBg==
-----END OPENSSH PRIVATE KEY-----
]]
local EC256_SEC1 = [[
-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIFTIVEk80oK6TrFbKljoMbutMYqucBpdsYa714B8lI5qoAoGCCqGSM49
AwEHoUQDQgAE1yCW7yQ9WUcJQne3Vbbph1QNhGzZ/vD/KrnE4zvcJ5F0doNuONlu
CA6mZJOaTcmDisnYM+fOq0TIVGBd2myAAw==
-----END EC PRIVATE KEY-----
]]
local EC256_SPKI = [[
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE1yCW7yQ9WUcJQne3Vbbph1QNhGzZ
/vD/KrnE4zvcJ5F0doNuONluCA6mZJOaTcmDisnYM+fOq0TIVGBd2myAAw==
-----END PUBLIC KEY-----
]]
local EC256_PUB = base64.decode("AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBNcglu8kPVlHCUJ3t1W26YdUDYRs2f7w/yq5xOM73CeRdHaDbjjZbggOpmSTmk3Jg4rJ2DPnzqtEyFRgXdpsgAM=")

local EC384 = [[
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAiAAAABNlY2RzYS
1zaGEyLW5pc3RwMzg0AAAACG5pc3RwMzg0AAAAYQRnezlrCoYBMLVRLKY0F5H41pXrNsgI
7o77MW5ZPKfGWhgkpRCEbPtto1wAj9cdAzF5OnzrfeNovLJRX+e50O+YMLUlGEvsDGuJjI
cCqwpkSZOc0Rf+c5GjsNhY4e20GxYAAADYK1ivrCtYr6wAAAATZWNkc2Etc2hhMi1uaXN0
cDM4NAAAAAhuaXN0cDM4NAAAAGEEZ3s5awqGATC1USymNBeR+NaV6zbICO6O+zFuWTynxl
oYJKUQhGz7baNcAI/XHQMxeTp8633jaLyyUV/nudDvmDC1JRhL7AxriYyHAqsKZEmTnNEX
/nORo7DYWOHttBsWAAAAMQDtV0CrCTYsp2wiWEu6lzB3uSYXgVBvRnVVsM33fVd1grXyLS
ntaZT3p7pCD3rPkPMAAAAKaHVsbC1lYzM4NAECAwQF
-----END OPENSSH PRIVATE KEY-----
]]
local EC384_SEC1 = [[
-----BEGIN EC PRIVATE KEY-----
MIGkAgEBBDDtV0CrCTYsp2wiWEu6lzB3uSYXgVBvRnVVsM33fVd1grXyLSntaZT3
p7pCD3rPkPOgBwYFK4EEACKhZANiAARnezlrCoYBMLVRLKY0F5H41pXrNsgI7o77
MW5ZPKfGWhgkpRCEbPtto1wAj9cdAzF5OnzrfeNovLJRX+e50O+YMLUlGEvsDGuJ
jIcCqwpkSZOc0Rf+c5GjsNhY4e20GxY=
-----END EC PRIVATE KEY-----
]]
local EC384_SPKI = [[
-----BEGIN PUBLIC KEY-----
MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEZ3s5awqGATC1USymNBeR+NaV6zbICO6O
+zFuWTynxloYJKUQhGz7baNcAI/XHQMxeTp8633jaLyyUV/nudDvmDC1JRhL7Axr
iYyHAqsKZEmTnNEX/nORo7DYWOHttBsW
-----END PUBLIC KEY-----
]]
local EC384_PUB = base64.decode("AAAAE2VjZHNhLXNoYTItbmlzdHAzODQAAAAIbmlzdHAzODQAAABhBGd7OWsKhgEwtVEspjQXkfjWles2yAjujvsxblk8p8ZaGCSlEIRs+22jXACP1x0DMXk6fOt942i8slFf57nQ75gwtSUYS+wMa4mMhwKrCmRJk5zRF/5zkaOw2Fjh7bQbFg==")

local EC521 = [[
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAArAAAABNlY2RzYS
1zaGEyLW5pc3RwNTIxAAAACG5pc3RwNTIxAAAAhQQBdJ+EL6DZ/b0MLtBZ6zXWa9ukFLaJ
lPHN2WjKz2fYyyHqA0mqAl+yT9VW5JfSJ9OO8LUaJTiQuh0tXPsY7kfmyT4A7PCPHjJ8/1
1Qfe+OOOQ21AmJKmS85SvYNO7hTdiN1BNAQIejfPV5brhiNef79f1nxC6acEZ+hwQ8Rngb
VmQwUmUAAAEIhUMfS4VDH0sAAAATZWNkc2Etc2hhMi1uaXN0cDUyMQAAAAhuaXN0cDUyMQ
AAAIUEAXSfhC+g2f29DC7QWes11mvbpBS2iZTxzdloys9n2Msh6gNJqgJfsk/VVuSX0ifT
jvC1GiU4kLodLVz7GO5H5sk+AOzwjx4yfP9dUH3vjjjkNtQJiSpkvOUr2DTu4U3YjdQTQE
CHo3z1eW64YjXn+/X9Z8QumnBGfocEPEZ4G1ZkMFJlAAAAQgD9JCT57f+yQzLU4ErwpFhC
nH0f/HHjfTAI5PO817vr0OKxkGcnJqzIeJ/LwiORPmNoXN+MdPLUgOWINu4uaj8f/AAAAA
podWxsLWVjNTIx
-----END OPENSSH PRIVATE KEY-----
]]

local EC256_ENC = [[
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAACmFlczI1Ni1jdHIAAAAGYmNyeXB0AAAAGAAAABCmeF/Lnj
9/D1gMS3KRSkf6AAAAGAAAAAEAAABoAAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlz
dHAyNTYAAABBBNcglu8kPVlHCUJ3t1W26YdUDYRs2f7w/yq5xOM73CeRdHaDbjjZbggOpm
STmk3Jg4rJ2DPnzqtEyFRgXdpsgAMAAACw9LHNnNi0TWChZfTLq/N8XUws/ah0wVBc+S6R
OTO4K9RWpjGU5VOJi4XfiFA73qWQtoSPdg/N7FWC66L6SfYlwYbaVHAWF1lrlnk8iDGhF0
88+i3SPLQ531tPejab6R+J5KlfSH/q613Vra76nD5d6OgFY6FIGej/isEizTn3LZFV745f
0/Knab863Tw3Dw2Y0zpbDVvRpbyUIcYule+wVgs8kJVLJBeZLWd5UOI6+Nk=
-----END OPENSSH PRIVATE KEY-----
]]
local PASSPHRASE = "correct horse"

local MSG = "hull ssh ecdsa user key test"

local has_crypto, crypto = pcall(require, "hull.crypto")
if not (has_crypto and type(crypto) == "table" and type(crypto.sign) == "function") then
    crypto = nil
end

-- A stand-in: the self-check signs and verifies; `verifies` decides the answer.
local function stub(verifies)
    return {
        sign = function() return "SIG" end,
        verify = function() return verifies end,
    }
end

test("an OpenSSH P-256 key loads, and its SEC1 PEM is OpenSSL's", function()
    local k = privatekey.load(EC256, { crypto = stub(true) })
    assert_eq(k.algorithm, "ecdsa-sha2-nistp256")
    assert_eq(k.curve, "nistp256")
    assert_eq(#k.q, 65)
    assert_eq(k.comment, "hull-ec256")
    assert_eq(k.blob, EC256_PUB, "the blob a server is shown:")
    assert_eq(k.pem, EC256_SEC1, "the PEM Hull builds:")
    assert_eq(k.d, nil, "the scalar is not kept outside the PEM:")
end)

test("an OpenSSH P-384 key loads, and its SEC1 PEM is OpenSSL's", function()
    local k = privatekey.load(EC384, { crypto = stub(true) })
    assert_eq(k.algorithm, "ecdsa-sha2-nistp384")
    assert_eq(#k.q, 97)
    assert_eq(k.blob, EC384_PUB)
    assert_eq(k.pem, EC384_SEC1)
end)

test("a P-521 key is refused with a code", function()
    local ok, err = pcall(privatekey.load, EC521, { crypto = stub(true) })
    assert_eq(ok, false)
    assert_eq(type(err) == "table" and err.code, "unsupported_key_type")
end)

test("a key whose private half does not sign for its public point is damaged", function()
    local ok, err = pcall(privatekey.load, EC256, { crypto = stub(false) })
    assert_eq(ok, false)
    assert_eq(tostring(err):find("ECDSA key is damaged", 1, true) ~= nil, true, tostring(err))
end)

test("an ECDSA signature goes on the wire as mpint r, mpint s", function()
    -- r with a leading zero byte (dropped), s with the top bit set (a zero
    -- byte added, or it would read as negative).
    local r = "\0" .. string.rep("\1", 31)
    local s = "\128" .. string.rep("\2", 31)
    local blob = userauth.signature_blob(r .. s, "ecdsa-sha2-nistp256")
    local rd = wire.reader(blob)
    assert_eq(rd:string(), "ecdsa-sha2-nistp256")
    local inner = wire.reader(rd:string())
    assert_eq(inner:uint32(), 31)
    assert_eq(inner:raw(31), string.rep("\1", 31))
    assert_eq(inner:uint32(), 33)
    assert_eq(inner:raw(33), "\0" .. s)
end)

if crypto then
    test("a P-256 key signs, and the signature verifies against ssh-keygen's export", function()
        local k = privatekey.load(EC256)
        local sig = crypto.sign("ES256", k.pem, MSG)
        assert_eq(#sig, 64)
        assert_eq(crypto.verify("ES256", EC256_SPKI, MSG, sig), true)
    end)

    test("a P-384 key signs, and the signature verifies", function()
        local k = privatekey.load(EC384)
        local sig = crypto.sign("ES384", k.pem, MSG)
        assert_eq(#sig, 96)
        assert_eq(crypto.verify("ES384", EC384_SPKI, MSG, sig), true)
    end)

    test("a passphrase-protected copy loads to the same key and signs", function()
        -- EC256_ENC is EC256 after `ssh-keygen -p`: same key, now encrypted.
        local k = privatekey.load(EC256_ENC, { passphrase = PASSPHRASE })
        assert_eq(k.blob, EC256_PUB)
        local sig = crypto.sign("ES256", k.pem, MSG)
        assert_eq(crypto.verify("ES256", EC256_SPKI, MSG, sig), true)
    end)
end

return { pass = pass, fail = fail }
