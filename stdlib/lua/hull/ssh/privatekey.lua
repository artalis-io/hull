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

-- Load a key from the contents of a key file.
--
-- Returns { algorithm, public, secret, comment, blob } where `blob` is the
-- wire-format public key ready for userauth, `public` is the raw 32 bytes and
-- `secret` the 64 bytes signing wants.
function M.load(text)
    local container = M.parse_container(M.unarmour(text))
    if container.encrypted then
        error("ssh.privatekey: the key is encrypted (cipher "
              .. container.cipher .. ", kdf " .. container.kdf
              .. "); Hull cannot decrypt it. Provide an unencrypted key, or "
              .. "remove the passphrase with: ssh-keygen -p -N \"\" -f <file>")
    end
    local key = parse_private(container.private_blob)

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
