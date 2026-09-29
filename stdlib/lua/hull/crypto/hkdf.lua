-- hull.crypto.hkdf - HKDF-SHA256 (RFC 5869): one secret in, several keys out.
--
-- For when a single secret has to feed more than one use - an encryption key
-- and a MAC key, or a key per tenant - without any two of them being the same
-- bytes. Using one key for two jobs is how a signature in one context becomes
-- valid in another; HKDF gives each job its own key, bound to a label (`info`)
-- that says what the key is for. The JS module (hull:crypto:hkdf) derives the
-- same bytes.
--
--   extract: PRK = HMAC-SHA256(salt, IKM)          salt defaults to 32 zeros
--   expand:  T(i) = HMAC-SHA256(PRK, T(i-1) || info || i), OKM = T(1) || ...
--
-- HKDF is for inputs that are already high-entropy (a random key, a
-- Diffie-Hellman output). It is NOT a password hash: a password needs a slow
-- function, which is crypto.hash_password.

local crypto = require('hull.crypto')

local M = {}

M.HASH_LEN = 32
M.MAX_LENGTH = 255 * M.HASH_LEN      -- RFC 5869 section 2.3

local function bytes_arg(name, v, what)
    if type(v) ~= "string" then
        error("hkdf." .. name .. ": " .. what .. " must be a byte string", 3)
    end
end

--- PRK = HMAC-SHA256(salt, ikm). An empty or absent salt is HashLen zeros,
--- as the RFC specifies (and HMAC takes no empty key).
function M.extract(salt, ikm)
    if salt == nil or salt == "" then salt = string.rep("\0", M.HASH_LEN) end
    bytes_arg("extract", salt, "salt")
    bytes_arg("extract", ikm, "input key material")
    return crypto.hmac_sha256(ikm, salt)
end

--- `length` bytes of output keying material from a PRK and a label.
function M.expand(prk, info, length)
    bytes_arg("expand", prk, "prk")
    if info == nil then info = "" end
    bytes_arg("expand", info, "info")
    length = math.tointeger(length)
    if not length or length < 1 or length > M.MAX_LENGTH then
        error("hkdf.expand: length must be 1.." .. M.MAX_LENGTH, 2)
    end
    if #prk < M.HASH_LEN then
        error("hkdf.expand: prk must be at least " .. M.HASH_LEN .. " bytes", 2)
    end
    local out, t = {}, ""
    local n = (length + M.HASH_LEN - 1) // M.HASH_LEN
    for i = 1, n do
        t = crypto.hmac_sha256(t .. info .. string.char(i), prk)
        out[i] = t
    end
    return table.concat(out):sub(1, length)
end

--- Extract then expand: `length` bytes derived from `ikm` for the use `info`
--- names. opts: { salt = bytes?, info = bytes? }.
---
---   local enc = hkdf.derive(master, 32, { info = "app v1 encryption" })
---   local mac = hkdf.derive(master, 32, { info = "app v1 mac" })
function M.derive(ikm, length, opts)
    opts = opts or {}
    if type(opts) ~= "table" then error("hkdf.derive: opts must be a table", 2) end
    return M.expand(M.extract(opts.salt, ikm), opts.info, length)
end

return M
