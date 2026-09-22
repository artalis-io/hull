-- test_ssh_privatekey.lua - Tests for hull.encoding.base64 and hull.ssh.privatekey
--
-- The key file is BUILT here rather than embedded, so the repository carries
-- no blob that looks like a private key to a secret scanner, and so each
-- malformation can be produced exactly.

local base64     = require('hull.encoding.base64')
local privatekey = require('hull.ssh.privatekey')
local wire       = require('hull.ssh.wire')

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

-- base64 --------------------------------------------------------------------

test("encode matches the RFC 4648 vectors", function()
    local cases = {
        { "", "" }, { "f", "Zg" }, { "fo", "Zm8" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg" }, { "fooba", "Zm9vYmE" }, { "foobar", "Zm9vYmFy" },
    }
    for _, c in ipairs(cases) do
        assert_eq(base64.encode_nopad(c[1]), c[2], "nopad " .. c[1])
    end
end)

test("padded encode matches the RFC 4648 vectors", function()
    local cases = {
        { "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
    }
    for _, c in ipairs(cases) do
        assert_eq(base64.encode(c[1]), c[2], "padded " .. c[1])
    end
end)

test("decode round trips every byte value", function()
    local all = {}
    for i = 0, 255 do all[#all + 1] = string.char(i) end
    local raw = table.concat(all)
    assert_eq(base64.decode(base64.encode(raw)), raw)
    assert_eq(base64.decode(base64.encode_nopad(raw)), raw)
end)

test("decode skips the line breaks in armoured files", function()
    -- Key files wrap at 70 columns, so this is the normal case, not an edge.
    assert_eq(base64.decode("Zm9v\nYmFy\r\n"), "foobar")
    assert_eq(base64.decode("Zm9v YmFy"), "foobar")
end)

test("decode refuses junk rather than skipping it", function()
    -- Silently ignoring stray bytes would let a corrupted key file decode to
    -- something plausible.
    assert_raises(function() base64.decode("Zm9v!YmFy") end, "punctuation")
    assert_raises(function() base64.decode("Zm9v-YmFy") end, "url-safe char")
end)

-- building a key file ---------------------------------------------------------

local PUB    = string.rep("\11", 32)
local SECRET = string.rep("\22", 32) .. PUB      -- seed || public
local COMMENT = "test@hull"

local function pub_blob(pub)
    return wire.writer():string("ssh-ed25519"):string(pub or PUB):build()
end

local function private_section(opts)
    opts = opts or {}
    local check = opts.check or 0x01020304
    local w = wire.writer()
    w:uint32(check)
    w:uint32(opts.check2 or check)
    w:string(opts.algo or "ssh-ed25519")
    w:string(opts.pub or PUB)
    w:string(opts.secret or SECRET)
    w:string(COMMENT)
    local body = w:build()
    -- padding is 1,2,3... up to the block size (8 for an unencrypted key)
    local pad = {}
    local i = 1
    while (#body + #pad) % 8 ~= 0 do pad[#pad + 1] = string.char(i); i = i + 1 end
    return body .. table.concat(pad)
end

local function key_file(opts)
    opts = opts or {}
    local w = wire.writer()
    w:raw(privatekey.MAGIC)
    w:string(opts.cipher or "none")
    w:string(opts.kdf or "none")
    w:string(opts.kdfopts or "")
    w:uint32(opts.nkeys or 1)
    w:string(opts.public_blob or pub_blob())
    w:string(opts.private_blob or private_section(opts))
    local armoured = base64.encode(w:build())
    return privatekey.BEGIN .. "\n" .. armoured .. "\n" .. privatekey.END .. "\n"
end

-- loading -----------------------------------------------------------------------

test("loads a well-formed key", function()
    local k = privatekey.load(key_file())
    assert_eq(k.algorithm, "ssh-ed25519")
    assert_eq(k.public, PUB)
    assert_eq(k.secret, SECRET)
    assert_eq(k.comment, COMMENT)
    assert_eq(k.blob, pub_blob())
end)

test("the secret half is passed through at 64 bytes", function()
    -- OpenSSH stores seed || public, which is exactly what signing wants;
    -- trimming it to 32 would break every signature.
    local k = privatekey.load(key_file())
    assert_eq(#k.secret, 64)
    assert_eq(k.secret:sub(33), k.public)
end)

test("refuses a PEM or PKCS8 key by name", function()
    -- The usual cause of this error is a key from another tool, so the
    -- message should say which format is missing rather than just "bad".
    local ok, err = pcall(privatekey.load,
        "-----BEGIN PRIVATE KEY-----\nMC4CAQ==\n-----END PRIVATE KEY-----\n")
    assert_eq(ok, false)
    assert_eq(tostring(err):find("PEM", 1, true) ~= nil, true, tostring(err))
end)

test("refuses a truncated file", function()
    assert_raises(function()
        privatekey.load(privatekey.BEGIN .. "\nZm9v\n")
    end, "no END line")
end)

test("refuses bad magic", function()
    local body = base64.encode("not-a-key-v1\0" .. string.rep("\0", 40))
    assert_raises(function()
        privatekey.load(privatekey.BEGIN .. "\n" .. body .. "\n" .. privatekey.END)
    end, "magic")
end)

-- encrypted keys -------------------------------------------------------------------

test("refuses an encrypted key and says why", function()
    -- Decrypting needs bcrypt_pbkdf, which Hull does not have. Producing
    -- garbage from a key the user believes is fine would be worse.
    local ok, err = pcall(privatekey.load,
        key_file({ cipher = "aes256-ctr", kdf = "bcrypt" }))
    assert_eq(ok, false)
    err = tostring(err)
    assert_eq(err:find("encrypted", 1, true) ~= nil, true, err)
    assert_eq(err:find("aes256%-ctr") ~= nil, true, err)
    -- and it tells the user what to do about it
    assert_eq(err:find("ssh%-keygen %-p") ~= nil, true, err)
end)

test("parse_container exposes an encrypted key without refusing", function()
    -- So a caller can report WHICH key needs a passphrase.
    local raw = privatekey.unarmour(key_file({ cipher = "aes256-ctr", kdf = "bcrypt" }))
    local c = privatekey.parse_container(raw)
    assert_eq(c.encrypted, true)
    assert_eq(c.cipher, "aes256-ctr")
end)

-- malformed keys ----------------------------------------------------------------------

test("refuses mismatched check bytes", function()
    -- For an encrypted key this is how a wrong passphrase shows up; here it
    -- means the file is damaged.
    local ok, err = pcall(privatekey.load,
        key_file({ private_blob = private_section({ check = 1, check2 = 2 }) }))
    assert_eq(ok, false)
    assert_eq(tostring(err):find("check bytes", 1, true) ~= nil, true, tostring(err))
end)

test("refuses an unsupported key type", function()
    local ok, err = pcall(privatekey.load,
        key_file({ private_blob = private_section({ algo = "ssh-rsa" }) }))
    assert_eq(ok, false)
    assert_eq(tostring(err):find("ssh%-rsa") ~= nil, true, tostring(err))
end)

test("refuses when the two public copies disagree", function()
    -- The key would sign with one identity and present another.
    local other = string.rep("\99", 32)
    local ok, err = pcall(privatekey.load, key_file({
        private_blob = private_section({ secret = string.rep("\22", 32) .. other }),
    }))
    assert_eq(ok, false)
    assert_eq(tostring(err):find("disagree", 1, true) ~= nil, true, tostring(err))
end)

test("refuses when the container blob disagrees with the private key", function()
    local ok, err = pcall(privatekey.load, key_file({
        public_blob = pub_blob(string.rep("\77", 32)),
    }))
    assert_eq(ok, false)
    assert_eq(tostring(err):find("does not match", 1, true) ~= nil, true, tostring(err))
end)

test("refuses a multi-key file", function()
    assert_raises(function()
        privatekey.load(key_file({ nkeys = 2 }))
    end, "two keys")
end)

test("refuses bad padding", function()
    -- Catches a truncated file that happened to parse.
    local body = private_section()
    local broken = body:sub(1, #body - 1) .. "\99"
    assert_raises(function()
        privatekey.load(key_file({ private_blob = broken }))
    end, "padding")
end)

test("refuses a wrong-length public half", function()
    assert_raises(function()
        privatekey.load(key_file({
            private_blob = private_section({ pub = string.rep("\11", 31) }),
        }))
    end, "31-byte public")
end)

-- fingerprint ----------------------------------------------------------------------------

test("fingerprint has the OpenSSH shape", function()
    local k = privatekey.load(key_file())
    local fp = privatekey.fingerprint(function() return string.rep("\0", 32) end, k)
    assert_eq(fp, "SHA256:" .. string.rep("A", 43))
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
