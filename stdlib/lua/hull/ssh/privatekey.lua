-- hull.ssh.privatekey - load an OpenSSH private key file.
--
-- The missing half of publickey authentication: hull.ssh.userauth can sign
-- with a key, and this is where one comes from. Parses the `openssh-key-v1`
-- container, which is OpenSSH's own format - NOT PEM or PKCS#8, despite the
-- PEM-looking armour around it. Inside, it is the same length-prefixed
-- encoding as everything else in SSH, so hull.ssh.wire reads it directly.
--
-- Encrypted keys are DECRYPTED when the caller says where the passphrase
-- comes from, and refused by name otherwise - see M.decrypt_private. Only
-- aes256-ctr with bcrypt is read, which is what ssh-keygen writes; anything
-- else is named in the error along with the conversion that fixes it, rather
-- than half-parsed into garbage from a key the user believes is fine.
--
-- WHAT IS AND IS NOT PROTECTED. `passphrase_env` keeps the PASSPHRASE out of
-- Lua entirely: C reads the variable, derives, and scrubs its buffers. It
-- does not, and cannot, do the same for what comes back. The derived key,
-- the counter block, and the decrypted private key are all ordinary Lua
-- strings - immutable, uncollectable on demand, unreachable by any scrub -
-- and the key must stay one for as long as it is used to sign. So the
-- guarantee is "the passphrase never becomes a Lua value", not "key material
-- is scrubbed end to end". Load a key once at the point of use, keep it for
-- as little of the program as possible, and do not pass it around.

local wire   = require('hull.ssh.wire')
local base64 = require('hull.encoding').base64

local M = {}

M.MAGIC      = "openssh-key-v1\0"
M.BEGIN      = "-----BEGIN OPENSSH PRIVATE KEY-----"
M.END        = "-----END OPENSSH PRIVATE KEY-----"
M.ALGORITHM  = "ssh-ed25519"
M.RSA        = "ssh-rsa"

-- The smallest RSA key this signs with: the floor the host key check uses
-- too. A server may well accept less; that is not a reason to.
M.RSA_MIN_BITS = 2048

-- Strip the armour and decode the body.
-- The failures a caller acts on differently carry a code: hull.ssh maps them
-- to its reason table rather than guessing from the message. The message is
-- still what tostring() gives, so logs and tests read the same text.
local Coded = { __tostring = function(e) return e.detail end }
local function raise(code, msg)
    error(setmetatable({ code = code, detail = msg }, Coded), 0)
end

function M.unarmour(text)
    if type(text) ~= "string" then
        error("ssh.privatekey: expected the file contents as a string", 2)
    end
    local b = text:find(M.BEGIN, 1, true)
    if not b then
        -- The most useful thing to say is which format this ISN'T, because
        -- the usual cause is a PEM key from another tool. Only something that
        -- looks like one is unsupported_key_type; anything else is a bad key.
        local msg = "ssh.privatekey: not an OpenSSH private key "
              .. "(no " .. M.BEGIN .. " line); PEM and PKCS#8 keys are not supported"
        if text:find("-----BEGIN ", 1, true) then raise("unsupported_key_type", msg) end
        error(msg, 0)
    end
    local e = text:find(M.END, b, true)
    if not e then
        error("ssh.privatekey: truncated key file (no END line)")
    end
    -- The armour wraps at 70 columns, so whitespace is skipped; anything else
    -- outside the alphabet is refused, because a damaged key file must not
    -- decode to something plausible.
    local raw = base64.decode(text:sub(b + #M.BEGIN, e - 1), { lenient = true })
    if not raw then error("ssh.privatekey: damaged key file (invalid base64)") end
    return raw
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
    local key
    if algo == M.ALGORITHM then
        local public = r:string()
        local secret = r:string()
        if #public ~= 32 then
            error("ssh.privatekey: public half is " .. tostring(#public)
                  .. " bytes, expected 32")
        end
        if #secret ~= 64 then
            -- OpenSSH stores seed || public as one 64-byte value, which is
            -- also what TweetNaCl signing wants, so it is passed through.
            error("ssh.privatekey: secret half is " .. tostring(#secret)
                  .. " bytes, expected 64")
        end
        -- The public half appears twice in the file. They must agree, or the
        -- key would sign with one identity and present another.
        if secret:sub(33) ~= public then
            error("ssh.privatekey: the two copies of the public key disagree")
        end
        key = { algorithm = algo, public = public, secret = secret }
    elseif algo == M.RSA then
        -- n, e, d, iqmp, p, q (sshkey_private_serialize). iqmp is dropped:
        -- the PEM is rebuilt from the rest, which also re-derives it.
        local n, e, d = r:mpint(), r:mpint(), r:mpint()
        r:mpint()
        local p, q = r:mpint(), r:mpint()
        key = { algorithm = algo, n = n, e = e, d = d, p = p, q = q }
    else
        raise("unsupported_key_type", "ssh.privatekey: unsupported key type "
              .. wire.safe_name(tostring(algo)) .. "; ssh-ed25519 and ssh-rsa are supported")
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

    key.comment = comment
    return key
end

local function rsa_bits(n)
    local top, bits = n:byte(1) or 0, 0
    while top > 0 do bits = bits + 1; top = top >> 1 end
    return (#n - 1) * 8 + bits
end

-- An RSA key's public blob, and the PEM hull.crypto.sign reads, built from
-- the components the file holds. The components are dropped from the key
-- afterwards: the PEM is what signs, and fewer copies of d, p and q is
-- better than more, even though none of them can be scrubbed (see the
-- header).
local function finish_rsa(key, opts)
    local bits = rsa_bits(key.n)
    if bits < M.RSA_MIN_BITS then
        raise("unsupported_key_type", "ssh.privatekey: RSA key of " .. tostring(bits)
              .. " bits is below the " .. tostring(M.RSA_MIN_BITS)
              .. "-bit minimum; generate a new one (ssh-keygen -t ed25519)")
    end
    local crypto = opts.crypto or require("hull.crypto")
    local ok, pem = pcall(crypto.rsa_private_pem, key.n, key.e, key.d, key.p, key.q)
    if not ok then
        error("ssh.privatekey: the RSA key is damaged (" .. tostring(pem) .. ")")
    end
    key.pem, key.bits = pem, bits
    key.public = wire.writer():string(M.RSA):mpint(key.e):mpint(key.n):build()
    key.d, key.p, key.q = nil, nil, nil
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
        raise("unsupported_key_type", "ssh.privatekey: unsupported protection (cipher "
              .. wire.safe_name(tostring(container.cipher)) .. ", kdf "
              .. wire.safe_name(tostring(container.kdf))
              .. "); Hull reads " .. M.CIPHER .. " with " .. M.KDF
              .. ". Convert it with: ssh-keygen -p -f <file>")
    end

    -- ORDER MATTERS. Ask "will the caller give me a passphrase" before
    -- "is this file well-formed", because the first is the likelier mistake
    -- and the more actionable message - and because there is no reason to
    -- parse a key we have already been told we cannot open.
    if not opts.passphrase_env and not opts.passphrase then
        raise("passphrase_required", "ssh.privatekey: the key is encrypted; pass a passphrase as "
              .. "opts.passphrase_env = \"VAR\" (preferred - the value never "
              .. "becomes a Lua string) or opts.passphrase = \"...\"")
    end
    if opts.passphrase == "" then
        raise("passphrase_required", "ssh.privatekey: the key is encrypted but the passphrase is empty")
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

    -- The round count is FILE content, and the derivation that follows runs
    -- to completion on the event loop: it cannot be interrupted, and each
    -- round costs milliseconds. A key declaring a few million rounds is not
    -- a slow key, it is a stalled process. Refused here as well as in C so
    -- the message can say which file did it.
    local max = crypto.BCRYPT_MAX_ROUNDS
    if max and rounds > max then
        error("ssh.privatekey: the key declares " .. tostring(rounds)
              .. " KDF rounds, above the " .. tostring(max) .. " limit "
              .. "(ssh-keygen writes 16, or 24 with -a); refusing to derive")
    end

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
-- Returns { algorithm, comment, blob } plus what signing needs, where `blob`
-- is the wire-format public key ready for userauth:
--   ssh-ed25519  `public` (the raw 32 bytes), `secret` (the 64 signing wants)
--   ssh-rsa      `pem` (PKCS#1, for hull.crypto.sign), `bits`, `n`, `e`
-- An RSA key needs hull.crypto (opts.crypto in tests) to build its PEM.
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
            raise("bad_passphrase", "ssh.privatekey: wrong passphrase (or the key is damaged)")
        end
        error(key)
    end

    -- The public blob in the container header is the one a server sees; check
    -- it against the private section rather than trusting either alone.
    local expect
    if key.algorithm == M.RSA then
        finish_rsa(key, opts or {})
        expect = key.public
    else
        expect = wire.writer():string(key.algorithm):string(key.public):build()
    end
    if container.public_blob ~= expect then
        error("ssh.privatekey: the public blob does not match the private key")
    end
    key.blob = container.public_blob
    return key
end

-- The fingerprint of the key, for showing which identity is being offered:
-- that of its public blob, the same form a host key's has.
function M.fingerprint(sha256_raw, key)
    return require('hull.ssh.hostkey').fingerprint(sha256_raw, key.blob)
end

return M
