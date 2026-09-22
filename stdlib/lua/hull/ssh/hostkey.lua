-- hull.ssh.hostkey - host key parsing, fingerprints, and the trust decision.
--
-- This is the module that decides whether Hull is talking to the machine it
-- meant to talk to. Everything else in hull/ssh is mechanism; this is policy.
--
-- Two rules shape it.
--
-- The key is exposed BEFORE trust. verify() hands back the fingerprint and a
-- status and makes no decision of its own, so an application can show an
-- operator what it is about to trust rather than being told afterwards.
--
-- The store is the APPLICATION's. Hull reads no ~/.ssh/known_hosts and writes
-- no file: a fleet tool trusts the machines its manifest names, and a global
-- file shared with an interactive ssh client is the wrong scope for that. The
-- caller passes a table with get/put/forget and owns where it lives.

local wire = require('hull.ssh.wire')

local M = {}

M.ALGORITHM = "ssh-ed25519"
M.KEY_LEN   = 32
M.SIG_LEN   = 64

-- Key and signature blobs -----------------------------------------------

-- An ssh-ed25519 public key blob (RFC 8709 section 4):
--   string "ssh-ed25519"
--   string key (32 bytes)
function M.parse_key(blob)
    local r = wire.reader(blob)
    local algo = r:string()
    if algo ~= M.ALGORITHM then
        -- Only Ed25519 is negotiated, so anything else means the server sent
        -- a key for an algorithm we did not agree to.
        error("ssh.hostkey: unsupported host key algorithm: "
              .. wire.safe_name(algo))
    end
    local key = r:string()
    if #key ~= M.KEY_LEN then
        error("ssh.hostkey: Ed25519 key is " .. tostring(#key)
              .. " bytes, expected 32")
    end
    return { algorithm = algo, key = key }
end

-- An ssh-ed25519 signature blob (RFC 8709 section 6):
--   string "ssh-ed25519"
--   string signature (64 bytes)
function M.parse_signature(blob)
    local r = wire.reader(blob)
    local algo = r:string()
    if algo ~= M.ALGORITHM then
        error("ssh.hostkey: unsupported signature algorithm: "
              .. wire.safe_name(algo))
    end
    local sig = r:string()
    if #sig ~= M.SIG_LEN then
        error("ssh.hostkey: Ed25519 signature is " .. tostring(#sig)
              .. " bytes, expected 64")
    end
    return { algorithm = algo, signature = sig }
end

-- Fingerprints ------------------------------------------------------------

-- Standard base64, not the URL-safe alphabet hull.crypto offers: a
-- fingerprint is read aloud and compared against ssh-keygen output, so
-- "nearly base64" is worse than useless. The implementation moved to
-- hull.ssh.base64 once the key loader needed the decoder too; re-exported
-- here because it is part of this module's tested surface.
local base64_nopad = require('hull.ssh.base64').encode_nopad

M.base64_nopad = base64_nopad

-- The OpenSSH fingerprint of a host key blob: SHA256:<base64 of the digest>,
-- no padding. Byte-for-byte what `ssh-keygen -l` prints, so an operator can
-- compare the two without translating between formats.
--
-- `sha256_raw` takes bytes and returns the 32 raw digest bytes.
function M.fingerprint(sha256_raw, blob)
    if type(sha256_raw) ~= "function" then
        error("ssh.hostkey: fingerprint needs a sha256 function", 2)
    end
    return "SHA256:" .. base64_nopad(sha256_raw(blob))
end

-- Verification --------------------------------------------------------------

-- Verify the server signature over the exchange hash.
--
-- `crypto` needs ed25519_verify(data, sig_hex, pk_hex) and to_hex, matching
-- hull.crypto plus hull.ssh.kex. Returns true, or false plus a reason.
function M.verify_signature(crypto, to_hex, key_blob, sig_blob, h)
    local ok, key = pcall(M.parse_key, key_blob)
    if not ok then return false, tostring(key) end
    local sig; ok, sig = pcall(M.parse_signature, sig_blob)
    if not ok then return false, tostring(sig) end

    if crypto.ed25519_verify(h, to_hex(sig.signature), to_hex(key.key)) then
        return true
    end
    -- The signature is over the exchange hash, which binds the host key to
    -- THIS exchange. A failure here means the peer does not hold the private
    -- half of the key it presented.
    return false, "ssh: host key signature does not verify"
end

-- Trust ---------------------------------------------------------------------

M.TRUSTED = "trusted"   -- stored key matches the one presented
M.UNKNOWN = "unknown"   -- no stored key for this host
M.CHANGED = "changed"   -- a key is stored and it is NOT this one

-- Compare the presented key against the store. Makes no decision: returns the
-- status so the caller can apply its own policy to UNKNOWN, and see CHANGED
-- for what it is.
function M.check(store, host, key_blob)
    if type(store) ~= "table" or type(store.get) ~= "function" then
        error("ssh.hostkey: a store with a get function is required", 2)
    end
    local stored = store.get(host)
    if stored == nil then return M.UNKNOWN, nil end
    if stored == key_blob then return M.TRUSTED, stored end
    return M.CHANGED, stored
end

-- Record a key for a host not seen before.
--
-- Refuses to overwrite. Trust-on-first-use is exactly that: FIRST use. An
-- accept that silently replaced an existing key would turn every
-- man-in-the-middle into a successful one, so replacing a key is a separate,
-- deliberate act (forget then accept) rather than a flag on this call.
function M.accept_new(store, host, key_blob)
    if type(store.put) ~= "function" then
        error("ssh.hostkey: the store has no put function", 2)
    end
    if store.get(host) ~= nil then
        error("ssh.hostkey: " .. tostring(host)
              .. " already has a stored key; forget it first")
    end
    store.put(host, key_blob)
    return true
end

-- Drop a stored key, so a genuinely rotated host can be re-accepted.
function M.forget(store, host)
    if type(store.forget) ~= "function" then
        error("ssh.hostkey: the store has no forget function", 2)
    end
    store.forget(host)
    return true
end

-- The whole host-key step: verify the signature, then report trust.
--
-- Returns a table the caller acts on. It NEVER decides to trust an unknown or
-- changed host itself, and there is deliberately no callback to let it: an
-- on_unknown hook is a thing applications wire to "return true" once and
-- forget, which is the same as having no trust store at all.
--
--   { ok = false, reason = ... }                    signature did not verify
--   { ok = true, status = "trusted", fingerprint }  proceed
--   { ok = true, status = "unknown", fingerprint }  caller decides, then
--                                                   accept_new to remember
--   { ok = true, status = "changed", fingerprint,
--     stored_fingerprint }                          caller MUST NOT proceed
--                                                   without human involvement
function M.verify(crypto, to_hex, sha256_raw, store, host, key_blob, sig_blob, h)
    local ok, reason = M.verify_signature(crypto, to_hex, key_blob, sig_blob, h)
    if not ok then
        return { ok = false, reason = reason }
    end

    local status, stored = M.check(store, host, key_blob)
    local out = {
        ok = true,
        status = status,
        fingerprint = M.fingerprint(sha256_raw, key_blob),
        key_blob = key_blob,
    }
    if status == M.CHANGED then
        -- Both fingerprints, because the useful question for whoever is
        -- reading is which key they were expecting.
        out.stored_fingerprint = M.fingerprint(sha256_raw, stored)
    end
    return out
end

return M
