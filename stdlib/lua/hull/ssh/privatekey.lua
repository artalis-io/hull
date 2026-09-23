-- hull.ssh.privatekey - load an OpenSSH private key file.
--
-- The missing half of publickey authentication: hull.ssh.userauth can sign
-- with a key, and this is where one comes from. Parses the `openssh-key-v1`
-- container, which is OpenSSH's own format - NOT PEM or PKCS#8, despite the
-- PEM-looking armour around it. Inside, it is the same length-prefixed
-- encoding as everything else in SSH, so hull.ssh.wire reads it directly.
--
-- Encrypted keys are REFUSED with a message naming the reason, rather than
-- half-parsed. Decrypting one needs bcrypt_pbkdf, which Hull does not have;
-- producing garbage from a key the user believes is fine would be worse than
-- saying so.
--
-- This handles private key material. Lua strings are immutable and garbage
-- collected, so a caller cannot scrub them - keep the loaded key for as
-- little of the program as possible, and prefer loading it once at the point
-- of use over passing it around.

local wire   = require('hull.ssh.wire')
local base64 = require('hull.encoding.base64')

local M = {}

M.MAGIC      = "openssh-key-v1\0"
M.BEGIN      = "-----BEGIN OPENSSH PRIVATE KEY-----"
M.END        = "-----END OPENSSH PRIVATE KEY-----"
M.ALGORITHM  = "ssh-ed25519"

-- Strip the armour and decode the body.
function M.unarmour(text)
    if type(text) ~= "string" then
        error("ssh.privatekey: expected the file contents as a string", 2)
    end
    local b = text:find(M.BEGIN, 1, true)
    if not b then
        -- The most useful thing to say is which format this ISN'T, because
        -- the usual cause is a PEM key from another tool.
        error("ssh.privatekey: not an OpenSSH private key "
              .. "(no " .. M.BEGIN .. " line); PEM and PKCS#8 keys are not supported")
    end
    local e = text:find(M.END, b, true)
    if not e then
        error("ssh.privatekey: truncated key file (no END line)")
    end
    local body = text:sub(b + #M.BEGIN, e - 1)
    return base64.decode(body)
end

-- Parse the container. Returns the fields without interpreting the key blob,
-- so a caller can inspect an encrypted key rather than only being refused.
function M.parse_container(raw)
    if raw:sub(1, #M.MAGIC) ~= M.MAGIC then
        error("ssh.privatekey: bad magic; this is not an openssh-key-v1 file")
    end
    local r = wire.reader(raw:sub(#M.MAGIC + 1))
    local out = {}
    out.cipher  = r:string()
    out.kdf     = r:string()
    out.kdfopts = r:string()
    out.nkeys   = r:uint32()
    if out.nkeys ~= 1 then
        -- The format allows several; ssh-keygen has never written more than
        -- one, and guessing which to use is not this module's decision.
        error("ssh.privatekey: file holds " .. tostring(out.nkeys)
              .. " keys, expected exactly 1")
    end
    out.public_blob = r:string()
    out.private_blob = r:string()
    out.encrypted = out.cipher ~= "none" or out.kdf ~= "none"
    return out
end

-- Parse the private section. Only valid for an unencrypted key.
local function parse_private(blob)
    local r = wire.reader(blob)
    local check1 = r:uint32()
    local check2 = r:uint32()
    if check1 ~= check2 then
        -- For an encrypted key this mismatch is how a wrong passphrase shows
        -- up. For an unencrypted one it means the file is damaged.
        error("ssh.privatekey: check bytes do not match; the file is corrupt")
    end

    local algo = r:string()
    if algo ~= M.ALGORITHM then
        error("ssh.privatekey: unsupported key type " .. tostring(algo)
              .. "; only " .. M.ALGORITHM .. " is supported")
    end

    local public = r:string()
    local secret = r:string()
    if #public ~= 32 then
        error("ssh.privatekey: public half is " .. tostring(#public)
              .. " bytes, expected 32")
    end
    if #secret ~= 64 then
        -- OpenSSH stores seed || public as one 64-byte value, which is also
        -- what TweetNaCl signing wants, so it is passed through unchanged.
        error("ssh.privatekey: secret half is " .. tostring(#secret)
              .. " bytes, expected 64")
    end
    -- The public half appears twice in the file. They must agree, or the key
    -- would sign with one identity and present another.
    if secret:sub(33) ~= public then
        error("ssh.privatekey: the two copies of the public key disagree")
    end

    local comment = r:string()

    -- Padding is 1,2,3... to the cipher block size. Checking it catches a
    -- truncated file that happened to parse.
    local i = 1
    while r:remaining() > 0 do
        local b = r:byte()
        if b ~= (i & 0xFF) then
            error("ssh.privatekey: bad padding at offset " .. tostring(i))
        end
        i = i + 1
    end

    return { algorithm = algo, public = public, secret = secret,
             comment = comment }
end

-- The one cipher this accepts, and the one KDF. ssh-keygen writes exactly
-- this pair today; anything else is refused BY NAME with the conversion that
-- fixes it, rather than by a generic failure the reader has to decode.
M.CIPHER = "aes256-ctr"
M.KDF    = "bcrypt"

-- aes256-ctr takes a 32-byte key and a 16-byte counter block, derived as ONE
-- 48-byte draw. bcrypt_pbkdf stripes its output, so two draws of 32 and 16
-- would not give the same bytes as one of 48 - the key would not open.
local CIPHER_KEY_LEN = 32
local CIPHER_IV_LEN  = 16

--- Decrypt the private section of a passphrase-protected key.
---
--- The passphrase can arrive two ways, and they are not equivalent:
---
---   opts.passphrase_env = "VAR"   the NAME of an environment variable. The
---                                 C layer reads the value, derives from it
---                                 and scrubs its copy - the passphrase never
---                                 becomes a Lua value. Prefer this.
---   opts.passphrase     = "..."   the bytes. Convenient, and unscrubbable:
---                                 Lua strings are immutable and interned, so
---                                 this one lives in the script heap until GC
---                                 and Hull cannot reach it.
---
--- Raises on a wrong passphrase, a refused cipher, or a damaged file. The
--- wrong-passphrase case is detected by parse_private's check1 == check2,
--- which is the only integrity signal the format has: CTR is unauthenticated,
--- so a wrong key yields plausible-looking garbage rather than an error.
function M.decrypt_private(container, opts)
    opts = opts or {}
    if container.cipher ~= M.CIPHER or container.kdf ~= M.KDF then
        error("ssh.privatekey: unsupported protection (cipher "
              .. tostring(container.cipher) .. ", kdf " .. tostring(container.kdf)
              .. "); Hull reads " .. M.CIPHER .. " with " .. M.KDF
              .. ". Convert it with: ssh-keygen -p -f <file>")
    end

    -- ORDER MATTERS. Ask "will the caller give me a passphrase" before
    -- "is this file well-formed", because the first is the likelier mistake
    -- and the more actionable message - and because there is no reason to
    -- parse a key we have already been told we cannot open.
    if not opts.passphrase_env and not opts.passphrase then
        error("ssh.privatekey: the key is encrypted; pass a passphrase as "
              .. "opts.passphrase_env = \"VAR\" (preferred - the value never "
              .. "becomes a Lua string) or opts.passphrase = \"...\"")
    end
    if opts.passphrase == "" then
        error("ssh.privatekey: the key is encrypted but the passphrase is empty")
    end

    -- kdfoptions is itself a length-prefixed blob: salt, then rounds. Read
    -- under pcall because it is FILE content: a truncated or absent one
    -- should read as a malformed key, not as a wire-layer error about byte
    -- counts that tells the reader nothing about which file is wrong.
    local okk, salt, rounds = pcall(function()
        local ko = wire.reader(container.kdfopts)
        return ko:string(), ko:uint32()
    end)
    if not okk then
        error("ssh.privatekey: the key declares " .. M.KDF .. " but its "
              .. "kdfoptions are malformed (expected a salt and a round count)")
    end
    if #salt == 0 then
        error("ssh.privatekey: the key carries an empty KDF salt")
    end
    if rounds == 0 then
        error("ssh.privatekey: the key declares zero KDF rounds")
    end

    local crypto = opts.crypto or require("hull.crypto")
    local want = CIPHER_KEY_LEN + CIPHER_IV_LEN

    -- passphrase_env wins when both are given: it is the safer of the two,
    -- and silently preferring the weaker one would be the wrong surprise.
    local derived
    if opts.passphrase_env then
        derived = crypto.bcrypt_pbkdf_env(opts.passphrase_env, salt, rounds, want)
    else
        derived = crypto.bcrypt_pbkdf(opts.passphrase, salt, rounds, want)
    end

    local ok, plain = pcall(function()
        return crypto.aes256ctr(derived:sub(1, CIPHER_KEY_LEN),
                                derived:sub(CIPHER_KEY_LEN + 1, want),
                                container.private_blob)
    end)
    -- Nothing derived from the passphrase outlives this function by name. The
    -- Lua copy cannot be wiped, but dropping the reference is what lets it go
    -- at the next collection rather than living as long as the key does.
    derived = nil
    if not ok then error(plain) end
    return plain
end

-- Load a key from the contents of a key file.
--
-- Returns { algorithm, public, secret, comment, blob } where `blob` is the
-- wire-format public key ready for userauth, `public` is the raw 32 bytes and
-- `secret` the 64 bytes signing wants.
--- @param opts table|nil { passphrase = string } or { passphrase_env = string }
function M.load(text, opts)
    local container = M.parse_container(M.unarmour(text))
    local blob = container.private_blob
    if container.encrypted then
        blob = M.decrypt_private(container, opts)
    end
    -- A wrong passphrase does not fail in the cipher - CTR is unauthenticated,
    -- so it decrypts to plausible garbage - it fails in parse_private, as
    -- check1 ~= check2. Reporting that as "the file is corrupt" would send
    -- someone to look for a damaged file when they simply mistyped, so the
    -- encrypted case names what actually happened.
    local ok, key = pcall(parse_private, blob)
    if not ok then
        if container.encrypted then
            error("ssh.privatekey: wrong passphrase (or the key is damaged)")
        end
        error(key)
    end

    -- The public blob in the container header is the one a server sees; check
    -- it against the private section rather than trusting either alone.
    if container.public_blob ~= wire.writer():string(key.algorithm)
                                             :string(key.public):build() then
        error("ssh.privatekey: the public blob does not match the private key")
    end
    key.blob = container.public_blob
    return key
end

-- The fingerprint of the key, for showing which identity is being offered.
function M.fingerprint(sha256_raw, key)
    return "SHA256:" .. base64.encode_nopad(sha256_raw(key.blob))
end

return M
