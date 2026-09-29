-- test_ssh_rsa_key.lua - RSA user keys, against keys ssh-keygen wrote
--
-- USER_RSA is `ssh-keygen -t rsa -b 2048`; USER_RSA_ENC is the same key after
-- `ssh-keygen -p` with the passphrase below (bcrypt + aes256-ctr, the format
-- a real passphrase-protected key has). SIG_512 / SIG_256 are OpenSSL's
-- signatures over MSG with that same key (converted with `ssh-keygen -m PEM`,
-- then `openssl dgst -sha512|-sha256 -sign`). RSA PKCS#1 v1.5 is
-- deterministic, so Hull signing with the key it loaded from the OpenSSH file
-- must produce exactly those bytes: that checks the parse, the rebuilt
-- private key (the CRT values are derived, not read) and the signature in one.
--
-- The vanilla harness runs the structural half with a stand-in crypto; the
-- caps-bearing leg in test_lua.c runs it all with the real hull.crypto and
-- requires the full count.

local privatekey = require('hull.ssh.privatekey')
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

local USER_RSA = [[
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAABFwAAAAdzc2gtcn
NhAAAAAwEAAQAAAQEAsSTLYc8M/ovN3quKNnnHBq7+TStYfIWAqp0DEtjuJaCd8QsbxSda
JDx6kJCBMCVRJtr4SFEIUWn7vT3wU5aYkj9CVGpPa6cxBDkzgwsZXd9RVL+PeqcdVryR5f
BB0JmTveH74hg6af1twCkKxmWefb99aHvU56Iuo/0d/7KonbkjUfvZMXqe5n0otML0dDUP
be2Wr+v3TYu1sGJqOKgb3utlZAVyNsQh7sL/hjx9iGW012/rsB/hh1g+d1YTUItwv6KIfV
4y+libse08I6Sd2BejCgSsVCqUBscDpHQuMgCZrL4zP/znGv5Ckk+znoAjlgN3/ApfGMY4
iUm12G2KfwAAA8DgG2CQ4BtgkAAAAAdzc2gtcnNhAAABAQCxJMthzwz+i83eq4o2eccGrv
5NK1h8hYCqnQMS2O4loJ3xCxvFJ1okPHqQkIEwJVEm2vhIUQhRafu9PfBTlpiSP0JUak9r
pzEEOTODCxld31FUv496px1WvJHl8EHQmZO94fviGDpp/W3AKQrGZZ59v31oe9Tnoi6j/R
3/sqiduSNR+9kxep7mfSi0wvR0NQ9t7Zav6/dNi7WwYmo4qBve62VkBXI2xCHuwv+GPH2I
ZbTXb+uwH+GHWD53VhNQi3C/ooh9XjL6WJux7TwjpJ3YF6MKBKxUKpQGxwOkdC4yAJmsvj
M//Oca/kKST7OegCOWA3f8Cl8YxjiJSbXYbYp/AAAAAwEAAQAAAQASIq+1wEJWzw5RFSPN
xvLi0siTMvRYrzxIe1GkvNPIw2RYGKiLqyc13YE4yqqHONLRlQplMPKWjzCoI662iAzvsA
1E4itmXJQOmCKXcGv06deEQyZXysMcraqMLWg3iHmRL5EfiRZ14m25hholzKKIwjF/IGpg
GoEG5fOLyYksuGsyiTAb8m3VpcUvP01IMyC1P4LzJJ8OLvNpny3daJZrtu7xfC4CZb9A2G
g9gPYBpXC/RSzqm7gqgITu5h6I8gh53tP452Lrw/0eoyooExZcy+cpjTOTawUKGl6UhShJ
ow1MZUNnkCfMrPjOmIOeztyi6P+Pi7IC8z1xmUHM9awRAAAAgQCQWpX8kSfNTWMvcreGzG
A5s5tTgRhedx7d8sDq5dQkhJxqQhN1pP3ins75n/RgDgRqdjf+J6fjN0vkmYmG3DbxVY5X
yXrPmlAvMST7c/J/2K6VBbEIxZ/JL2ynDci0tpCqdziICWf6Qa34f1Fz0g4rnD35zKXWdV
FXZTjaxhOrSwAAAIEA7VjTh7nLS6+KgdBjji0mHPxHmi74tyxlwvebsLFG4+H7oWu0pEDn
vSSxoBlCdbjDhZ0WT0OMNk+EgqJLFeFLf+Dg40FxjgVC0BpoK+NAe7LIfDm8BWIe1ItNSM
O243GKAT9Fchh3TYzK72b8J6oqv6tk21Pbxm1d3XCodhGmgFEAAACBAL8QvddxcfE2AlIm
REBQPPI9I5DEgCr65dNCpTTI5LnWKGGcIPpGOW22yyU+MO9ZhvcOUoHRwiGHH/g3IxZY42
/8mdsedIU+sHXaZPbLCV2Jx6VuppWjQKHZs9BTO6sQANBYdxByYSEH3a/dJ2SuSCv4mSNI
4WgrT+v1D9hF7fnPAAAACWh1bGwtdGVzdAE=
-----END OPENSSH PRIVATE KEY-----
]]

local USER_RSA_ENC = [[
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAACmFlczI1Ni1jdHIAAAAGYmNyeXB0AAAAGAAAABC9Ag6UQ7
p5gBH+tcI3PO7IAAAAGAAAAAEAAAEXAAAAB3NzaC1yc2EAAAADAQABAAABAQCxJMthzwz+
i83eq4o2eccGrv5NK1h8hYCqnQMS2O4loJ3xCxvFJ1okPHqQkIEwJVEm2vhIUQhRafu9Pf
BTlpiSP0JUak9rpzEEOTODCxld31FUv496px1WvJHl8EHQmZO94fviGDpp/W3AKQrGZZ59
v31oe9Tnoi6j/R3/sqiduSNR+9kxep7mfSi0wvR0NQ9t7Zav6/dNi7WwYmo4qBve62VkBX
I2xCHuwv+GPH2IZbTXb+uwH+GHWD53VhNQi3C/ooh9XjL6WJux7TwjpJ3YF6MKBKxUKpQG
xwOkdC4yAJmsvjM//Oca/kKST7OegCOWA3f8Cl8YxjiJSbXYbYp/AAADwBpUNxvuCOT5jk
bL04AtHj21x7D6kgg1AHNxb6XD6WZVIT9G3u87jdPWllo8HDmqLNnEBXUJOvqQ8JRLSp0Y
eG7vApUvSUI0zYZlRGcHnQfrCFBsixHOvvudWhoPZiScpy9CWOy5KZcNYpufx8zneNjdva
C91ZmoIzRFXW+WLEP0qaDDRCkJ8044gTww3uxTDAlnut2Y+IzHuTJSg1duPOD1hHwLFsTg
DcDmkEqJkT3OBWxS0egy+wXTQXBl4uJL4LzPSS+wAWVOsFBmQjKMAXo6t3gHUQxLw4YUnz
/Lrio8nf/6ZpJyi6OY3PFlLfUVeviI9oC+Of4Kw3rZMCaTh2RX9kTy/XtFOb1zjAzSnfwM
9y9A9o+rAyAbUHJMsZxMx1bL0UA8HiePsRYpJSqLcyQedh9+qAIvTgqawEupSzwpGQbLDl
DN298RK991eMiX1IY9QnWfJRSP86Yko9uJuk8g08Qrc50EbCZsB+S47kTj3g4dd1ZoYoss
gaPpUXuyRBFyqH0id9YBI1lSB7bdCl//XraeXmkEwhr3MCB9+cXUgwe2czyVurmMSP78RP
nVFXDSckMsA47mhfFJ0v0alQhJI+DkJfhr0HdD7PE+ZA1Thq+XTfMcMcAuBXIA1i/MweoH
Oi2gP1n5WjwkbFBeaYFesTCAkZubtMC9tSteTG3nde2nDo25qc8b3ZegbPF59KLi2vgLfk
ncGhy0GGm9LzbsOZQD5Al5FhxSjp4qw+uOz7yDjg6ePu+TOnd8dWpAE78ZqbXuLz3yoFjM
TKnQfPpWK9acCgaeX7b7GqLs48vLwHCMsJcpWMc4kKZ2baMJJY6fo5yuO+njMOF35eqkib
kjc8zS0VAr1lnltYnwrxozcYLVGBlF3sQsnS4c+9bKGcNu0hPDFAcebz3Q7IWN/qEMCN7J
8TZAMWu8ZNuW4K3TZwsBNPhhflC2Xeg+bh3HZe5uykHP0x+86z2r/ITaXTSCzfgpmqOsRC
rXva6umvkLcDaALyEI+VcEArhq1gMyUPmLZaCjYZIn9m2EgqVXSiSuO4IQ6Fav0TvbLXpY
qqT0zZNM2sbrfxmtkZHhGN72rzTwo4rzLig5uph+r0Si1bk2iAEsUTuEGcn34tJCEuX2Uk
vuUSqNOmM6ncTsO2EHTFuWyPz2Jp6imY3iFObXW8sXvaxOMMnohAXtrcyKQX07zGM+MI0i
p1HTR0BAJNCB7hfzpaksTgw3bsLeITrJ2SK7R/PBDgkkF2VEpS+HUtLHxbCBXYsyIlfC/a
LUIeBUrg==
-----END OPENSSH PRIVATE KEY-----
]]
local PASSPHRASE = "correct horse"

local USER_RSA_1024 = [[
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAlwAAAAdzc2gtcn
NhAAAAAwEAAQAAAIEAtftrWXB2+2LSMx5J7abZvP5LSUGkeW7xA2WplOqyyhF/38j4m0U/
0F7l5L5PNWpWCBNlJ5jdKZKPJiGGBfUN+AAlk6sUp/rsceDdn8TreMQzo7Sfu2rDIHcsRH
dVn0qocVeATPMCd+WWVK5WqPIWJ9AR2+dkNd6Pmm0NgzfUg9cAAAH4OOMg7TjjIO0AAAAH
c3NoLXJzYQAAAIEAtftrWXB2+2LSMx5J7abZvP5LSUGkeW7xA2WplOqyyhF/38j4m0U/0F
7l5L5PNWpWCBNlJ5jdKZKPJiGGBfUN+AAlk6sUp/rsceDdn8TreMQzo7Sfu2rDIHcsRHdV
n0qocVeATPMCd+WWVK5WqPIWJ9AR2+dkNd6Pmm0NgzfUg9cAAAADAQABAAAAgFW7s9A4Dq
XR4YtZmqSOdYf3GUrS1QSsgnkCPQk+JKrT4bVw/cZQsoadHptMGQ7mIM+/K2mjN5YtSKlD
I9vMnh5Lf+40KUP+Ob4v2pwnZDrt8LpIe37u/H4qI3jgdSBScbwudri8iXEJGrXVzPrLwb
L+gJ0oaEK2e0BvBhyv61wZAAAAQFvvJNso1U3stc8XIgamBTcZrfE7Knwi7kgAxN0Bpm37
Nr1liTHIwLE/70oKjxJhL34+/rxAIUeAbdEhEMnla48AAABBAOJ5sZgHDySQABkrs7Guve
QRIKbH6JtutzpvLlR+yBGz7EwAeBGIl+URITkZvb261YWS1KExeMQZuO+VGUz+iZsAAABB
AM200uRVvXTyTTOJMbtk13JVVkL8L8DqYu40Xio3+BoRoD9nqQIgY28wZWS27Fe3QngaSh
6+aHDmTlJw9krI4HUAAAAAAQID
-----END OPENSSH PRIVATE KEY-----
]]

local PUB_BLOB = base64.decode("AAAAB3NzaC1yc2EAAAADAQABAAABAQCxJMthzwz+i83eq4o2eccGrv5NK1h8hYCqnQMS2O4loJ3xCxvFJ1okPHqQkIEwJVEm2vhIUQhRafu9PfBTlpiSP0JUak9rpzEEOTODCxld31FUv496px1WvJHl8EHQmZO94fviGDpp/W3AKQrGZZ59v31oe9Tnoi6j/R3/sqiduSNR+9kxep7mfSi0wvR0NQ9t7Zav6/dNi7WwYmo4qBve62VkBXI2xCHuwv+GPH2IZbTXb+uwH+GHWD53VhNQi3C/ooh9XjL6WJux7TwjpJ3YF6MKBKxUKpQGxwOkdC4yAJmsvjM//Oca/kKST7OegCOWA3f8Cl8YxjiJSbXYbYp/")

local SPKI = [[
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsSTLYc8M/ovN3quKNnnH
Bq7+TStYfIWAqp0DEtjuJaCd8QsbxSdaJDx6kJCBMCVRJtr4SFEIUWn7vT3wU5aY
kj9CVGpPa6cxBDkzgwsZXd9RVL+PeqcdVryR5fBB0JmTveH74hg6af1twCkKxmWe
fb99aHvU56Iuo/0d/7KonbkjUfvZMXqe5n0otML0dDUPbe2Wr+v3TYu1sGJqOKgb
3utlZAVyNsQh7sL/hjx9iGW012/rsB/hh1g+d1YTUItwv6KIfV4y+libse08I6Sd
2BejCgSsVCqUBscDpHQuMgCZrL4zP/znGv5Ckk+znoAjlgN3/ApfGMY4iUm12G2K
fwIDAQAB
-----END PUBLIC KEY-----
]]

local MSG = "hull ssh rsa user key test"
local SIG_512 = base64.decode("leR60R0LMuWFhh51n//J2gEL7KdQeJWYuyeKyvaBRsnjlhPx/t1qMlv1srVvXg0ckAkYrhOiqyrJ3g++5qqVxjBxpYkzLy++qevhgaj47YXD81CeHcBmGE6Belf+1TClogU4x+cTRFN0tMUdDnY0LgDiVXiM0/WozMXuWpU3VbSZ/UfHEsSU8Q1E0tZKKmX6dCuRajmaIxWjgyBjkrfax0tid88AIfzmKtHYjk2UkPuiIBATHbDCWCo7rtKFJuK/LwxxcTHvoMr5Y699Bt5cbHNJove+sBYgh4ylBwsnk73/n1tx9b+fy4NOV8FFLS5zqZVtqYJilCWkhYNTZSn7NA==")
local SIG_256 = base64.decode("Bo+oPkr2i7urk13+dpF/Ai9/QO+ioCg/EpgK/aBrNuL0t4XHHHVkhTpUM8B0PASV/8dT9emZL955+/DacQv+J9+/KRmQgOCda0dbHN8lpM93tM6I741+Opv4YZPjcalomNzjOj6QVRmIE2Zxkemfmq6JPhpMf4TWyxjeOsZ9zuWH9dF3es4dzYpos6pg6bFGSeqpZ/i4ev6RJksUqsSR8ABToY9OMAsysF86j7ISkHCQEhcxVsjQrRDav/tmBhvrx/kbZB85Mcdkd4ha2lXWShSvOvNEC6CO4+dY6HPMA9thFmWsEi/BowkFFMPWFUjIQE27986qo5J2QM0l/r6kZg==")

local has_crypto, crypto = pcall(require, "hull.crypto")
if not (has_crypto and type(crypto) == "table" and type(crypto.sign) == "function") then
    crypto = nil
end

-- A stand-in that records what it was asked to build.
local function stub(fail_with)
    local seen = {}
    return {
        rsa_private_pem = function(n, e, d, p, q)
            if fail_with then error(fail_with) end
            seen.n, seen.e, seen.d, seen.p, seen.q = n, e, d, p, q
            return "PEM"
        end,
    }, seen
end

test("an OpenSSH RSA key loads, and is presented by its public blob", function()
    local c, seen = stub()
    local k = privatekey.load(USER_RSA, { crypto = c })
    assert_eq(k.algorithm, "ssh-rsa")
    assert_eq(k.bits, 2048)
    assert_eq(k.comment, "hull-test")
    assert_eq(k.blob, PUB_BLOB, "the blob a server is shown:")
    assert_eq(k.pem, "PEM")
    assert_eq(#seen.n, 256)
    assert_eq(seen.e, "\1\0\1")
    -- The components went into the PEM and are not kept on the key.
    assert_eq(k.d, nil)
    assert_eq(k.p, nil)
    assert_eq(k.q, nil)
end)

test("an RSA key below 2048 bits is refused with a code", function()
    local ok, err = pcall(privatekey.load, USER_RSA_1024, { crypto = stub() })
    assert_eq(ok, false)
    assert_eq(type(err) == "table" and err.code, "unsupported_key_type")
    assert_eq(tostring(err):find("2048", 1, true) ~= nil, true, tostring(err))
end)

test("components that do not form a key are reported as a damaged key", function()
    local ok, err = pcall(privatekey.load, USER_RSA,
                          { crypto = stub("the components do not form a valid RSA key") })
    assert_eq(ok, false)
    assert_eq(tostring(err):find("RSA key is damaged", 1, true) ~= nil, true, tostring(err))
end)

if crypto then
    test("the loaded key signs exactly as OpenSSL does with the same key", function()
        local k = privatekey.load(USER_RSA)
        assert_eq(crypto.sign("RS512", k.pem, MSG), SIG_512, "RS512")
        assert_eq(crypto.sign("RS256", k.pem, MSG), SIG_256, "RS256")
    end)

    test("a passphrase-protected RSA key loads to the same key", function()
        local k = privatekey.load(USER_RSA_ENC, { passphrase = PASSPHRASE })
        assert_eq(k.blob, PUB_BLOB)
        assert_eq(crypto.sign("RS512", k.pem, MSG), SIG_512)
    end)

    test("its signature verifies against the public key ssh-keygen exported", function()
        local k = privatekey.load(USER_RSA)
        assert_eq(crypto.verify("RS512", SPKI, MSG, crypto.sign("RS512", k.pem, MSG)), true)
    end)
end

return { pass = pass, fail = fail }
