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

local wire   = require('hull.ssh.wire')
local base64 = require('hull.encoding').base64

local M = {}

-- Ed25519: offered first, and verified by its own primitive. The constants
-- stay because callers (and the fuzz harness) use them.
M.ALGORITHM = "ssh-ed25519"
M.KEY_LEN   = 32
M.SIG_LEN   = 64

-- Every host key algorithm Hull verifies, by the name negotiated in KEXINIT.
--
-- `key_type` is the name inside the key blob. It equals the algorithm except
-- for RSA, where one "ssh-rsa" key signs under either SHA-2 hash (RFC 8332).
-- `jose` is the hull.crypto.verify name; Ed25519 has none because it goes
-- through crypto.ed25519_verify. `field` is the curve's coordinate size.
--
-- Deliberately absent: "ssh-rsa" as a SIGNATURE algorithm (SHA-1; OpenSSH
-- 8.8 disabled it) and ecdsa-sha2-nistp521 (hull.crypto has no ES512).
M.ALGORITHMS = {
    ["ssh-ed25519"]         = { key_type = "ssh-ed25519" },
    ["ecdsa-sha2-nistp256"] = { key_type = "ecdsa-sha2-nistp256", curve = "nistp256",
                                jose = "ES256", field = 32 },
    ["ecdsa-sha2-nistp384"] = { key_type = "ecdsa-sha2-nistp384", curve = "nistp384",
                                jose = "ES384", field = 48 },
    ["rsa-sha2-512"]        = { key_type = "ssh-rsa", jose = "RS512" },
    ["rsa-sha2-256"]        = { key_type = "ssh-rsa", jose = "RS256" },
}

-- The algorithms a key of `key_type` can be negotiated under, best first.
-- Used to put the key a trust store already holds for a host at the front of
-- the offer, so the server presents THAT key rather than another one it also
-- has - which would read as a changed host key.
function M.algorithms_for(key_type)
    if key_type == "ssh-rsa" then return { "rsa-sha2-512", "rsa-sha2-256" } end
    if M.ALGORITHMS[key_type] then return { key_type } end
    return {}
end

-- The key type a blob declares (its first string), or nil if it has none.
function M.key_type(blob)
    local ok, t = pcall(function() return wire.reader(blob):string() end)
    if ok then return t end
    return nil
end

-- RSA moduli Hull accepts. 2048 bits is the floor NIST sets and ssh-keygen's
-- default; a smaller host key is refused, not trusted. The ceiling only
-- bounds the work a hostile server can ask for.
M.RSA_MIN_BITS = 2048
M.RSA_MAX_BITS = 16384

-- DER, for the PEM that hull.crypto.verify reads ---------------------------

local function unhex(h)
    return (h:gsub("..", function(x) return string.char(tonumber(x, 16)) end))
end

local function der(tag, body)
    local n = #body
    local len
    if n < 0x80 then
        len = string.char(n)
    else
        local b = {}
        while n > 0 do table.insert(b, 1, string.char(n & 0xFF)); n = n >> 8 end
        len = string.char(0x80 | #b) .. table.concat(b)
    end
    return string.char(tag) .. len .. body
end

-- A DER INTEGER from an unsigned big-endian magnitude (no leading zeros, as
-- wire.mpint returns it): a zero byte in front when the top bit is set, or
-- the value would read as negative.
local function der_uint(mag)
    if mag == "" then mag = "\0" end
    if mag:byte(1) >= 0x80 then mag = "\0" .. mag end
    return der(0x02, mag)
end

-- SubjectPublicKeyInfo up to the BIT STRING contents, per curve (RFC 5480):
-- SEQUENCE { AlgorithmIdentifier { id-ecPublicKey, curve }, BIT STRING { 00 ...
local EC_SPKI_PREFIX = {
    nistp256 = unhex("3059301306072a8648ce3d020106082a8648ce3d030107034200"),
    nistp384 = unhex("3076301006072a8648ce3d020106052b81040022036200"),
}
-- AlgorithmIdentifier { rsaEncryption, NULL } (RFC 3279).
local RSA_ALG_ID = unhex("300d06092a864886f70d0101010500")

-- The curve OIDs a SEC1 private key names (RFC 5480 section 2.1.1.1).
local EC_CURVE_OID = {
    nistp256 = unhex("06082a8648ce3d030107"),
    nistp384 = unhex("06052b81040022"),
}

local function pem(der_bytes, label)
    label = label or "PUBLIC KEY"
    local b64 = base64.encode(der_bytes)
    local lines = {}
    for i = 1, #b64, 64 do lines[#lines + 1] = b64:sub(i, i + 63) end
    return "-----BEGIN " .. label .. "-----\n" .. table.concat(lines, "\n")
           .. "\n-----END " .. label .. "-----\n"
end

-- Key and signature blobs -----------------------------------------------

local function rsa_bits(n)
    local top, bits = n:byte(1), 0
    while top > 0 do bits = bits + 1; top = top >> 1 end
    return (#n - 1) * 8 + bits
end

-- A host key blob (RFC 8709 section 4, RFC 5656 section 3.1, RFC 4253 section
-- 6.6). Returns { type, algorithm } plus the key material: `key` (Ed25519),
-- `q` (the uncompressed ECDSA point) or `e` / `n` / `bits` (RSA).
function M.parse_key(blob)
    local r = wire.reader(blob)
    local t = r:string()
    local out = { type = t, algorithm = t }
    if t == M.ALGORITHM then
        out.key = r:string()
        if #out.key ~= M.KEY_LEN then
            error("ssh.hostkey: Ed25519 key is " .. tostring(#out.key)
                  .. " bytes, expected 32")
        end
    elseif t == "ecdsa-sha2-nistp256" or t == "ecdsa-sha2-nistp384" then
        local a = M.ALGORITHMS[t]
        local curve = r:string()
        if curve ~= a.curve then
            error("ssh.hostkey: " .. t .. " key names the curve "
                  .. wire.safe_name(curve))
        end
        out.q = r:string()
        -- Only the uncompressed form (0x04 || X || Y) is legal in SSH.
        if #out.q ~= 1 + 2 * a.field or out.q:byte(1) ~= 4 then
            error("ssh.hostkey: " .. t .. " point is not an uncompressed "
                  .. a.curve .. " point")
        end
    elseif t == "ssh-rsa" then
        out.e = r:mpint()
        out.n = r:mpint()
        if out.n == "" then error("ssh.hostkey: RSA modulus is zero") end
        out.bits = rsa_bits(out.n)
        if out.bits < M.RSA_MIN_BITS then
            error("ssh.hostkey: RSA host key of " .. tostring(out.bits)
                  .. " bits is below the " .. tostring(M.RSA_MIN_BITS) .. "-bit minimum")
        end
        if out.bits > M.RSA_MAX_BITS then
            error("ssh.hostkey: RSA host key of " .. tostring(out.bits)
                  .. " bits is above the " .. tostring(M.RSA_MAX_BITS) .. "-bit maximum")
        end
        if out.e == "" or out.e == "\1" or out.e:byte(-1) % 2 == 0 then
            error("ssh.hostkey: RSA public exponent must be odd and greater than 1")
        end
    else
        -- Anything else means the server sent a key for an algorithm we did
        -- not agree to, or one Hull does not verify.
        error("ssh.hostkey: unsupported host key algorithm: " .. wire.safe_name(t))
    end
    -- OpenSSH refuses trailing bytes too. A blob is compared byte for byte
    -- against the trust store, so two encodings of one key must not exist.
    if r:remaining() ~= 0 then
        error("ssh.hostkey: trailing bytes after the " .. t .. " key")
    end
    return out
end

-- A signature blob: string algorithm, string signature. For ECDSA the inner
-- string is itself mpint r, mpint s (RFC 5656 section 3.1.2); it comes back
-- here as the fixed-width r || s hull.crypto.verify takes, so a signature has
-- one form from this point on.
function M.parse_signature(blob)
    local r = wire.reader(blob)
    local algo = r:string()
    local sig = r:string()
    local a = M.ALGORITHMS[algo]
    if not a then
        error("ssh.hostkey: unsupported signature algorithm: " .. wire.safe_name(algo))
    end
    if algo == M.ALGORITHM then
        if #sig ~= M.SIG_LEN then
            error("ssh.hostkey: Ed25519 signature is " .. tostring(#sig)
                  .. " bytes, expected 64")
        end
    elseif a.field then
        local rs = wire.reader(sig)
        local sr, ss = rs:mpint(), rs:mpint()
        if rs:remaining() ~= 0 then
            error("ssh.hostkey: trailing bytes in the " .. algo .. " signature")
        end
        if sr == "" or ss == "" or #sr > a.field or #ss > a.field then
            error("ssh.hostkey: " .. algo .. " signature values do not fit the curve")
        end
        sig = string.rep("\0", a.field - #sr) .. sr .. string.rep("\0", a.field - #ss) .. ss
    elseif #sig == 0 then
        error("ssh.hostkey: empty " .. algo .. " signature")
    end
    if r:remaining() ~= 0 then
        error("ssh.hostkey: trailing bytes after the " .. algo .. " signature")
    end
    return { algorithm = algo, signature = sig }
end

-- The PEM SubjectPublicKeyInfo of a parsed ECDSA or RSA key: the form
-- hull.crypto.verify reads. Ed25519 keys have no PEM path; they are verified
-- directly.
function M.public_key_pem(key)
    if key.q then
        return pem(EC_SPKI_PREFIX[M.ALGORITHMS[key.type].curve] .. key.q)
    elseif key.n then
        local rsa = der(0x30, der_uint(key.n) .. der_uint(key.e))
        return pem(der(0x30, RSA_ALG_ID .. der(0x03, "\0" .. rsa)))
    end
    error("ssh.hostkey: a " .. tostring(key.type) .. " key has no PEM form", 2)
end

-- The SEC1 PEM ("EC PRIVATE KEY", RFC 5915) of the ECDSA key with public
-- point `q` and private scalar `d` (an unsigned big-endian magnitude, as an
-- OpenSSH key file's mpint reads), for hull.crypto.sign. Lives beside
-- public_key_pem so the DER for SSH keys is written in one place. The PEM is
-- secret; keep it only as long as signing needs it.
function M.ec_private_pem(curve, q, d)
    local a = M.ALGORITHMS["ecdsa-sha2-" .. tostring(curve)]
    if not a or not a.field or not EC_CURVE_OID[curve] then
        error("ssh.hostkey: no ECDSA curve " .. wire.safe_name(tostring(curve)), 2)
    end
    if type(q) ~= "string" or #q ~= 1 + 2 * a.field or q:byte(1) ~= 4 then
        error("ssh.hostkey: not an uncompressed " .. curve .. " point", 2)
    end
    if type(d) ~= "string" or d == "" or #d > a.field then
        error("ssh.hostkey: the ECDSA private scalar does not fit " .. curve, 2)
    end
    -- ECPrivateKey ::= SEQUENCE { version 1, privateKey OCTET STRING (the
    -- scalar at the field width), [0] parameters (the curve), [1] publicKey }
    local body = der(0x02, "\1")
              .. der(0x04, string.rep("\0", a.field - #d) .. d)
              .. der(0xA0, EC_CURVE_OID[curve])
              .. der(0xA1, der(0x03, "\0" .. q))
    return pem(der(0x30, body), "EC PRIVATE KEY")
end

-- Fingerprints ------------------------------------------------------------

-- Standard base64, unpadded, not the URL-safe alphabet: a fingerprint is
-- read aloud and compared against ssh-keygen output, so "nearly base64" is
-- worse than useless.
-- The OpenSSH fingerprint of a host key blob: SHA256:<base64 of the digest>,
-- no padding. Byte-for-byte what `ssh-keygen -l` prints, so an operator can
-- compare the two without translating between formats.
--
-- `sha256_raw` takes bytes and returns the 32 raw digest bytes.
function M.fingerprint(sha256_raw, blob)
    if type(sha256_raw) ~= "function" then
        error("ssh.hostkey: fingerprint needs a sha256 function", 2)
    end
    return "SHA256:" .. base64.encode(sha256_raw(blob), { pad = false })
end

-- Verification --------------------------------------------------------------

-- Verify the server signature over the exchange hash.
--
-- `alg` is the host key algorithm negotiated in KEXINIT. The signature must
-- be under exactly that algorithm, and the key must be of the type it
-- names: a server that signs under anything else is refused even when the
-- signature would verify, or it could choose its own (weaker) hash after
-- the negotiation settled on another. Without `alg` the signature's own
-- algorithm is taken, for callers that verify outside a key exchange.
--
-- `crypto` needs ed25519_verify(data, signature, public_key) for Ed25519 and
-- verify(jose_alg, pem, data, signature) for ECDSA and RSA, all bytes.
-- Returns true, or false plus a reason.
function M.verify_signature(crypto, key_blob, sig_blob, h, alg)
    local ok, key = pcall(M.parse_key, key_blob)
    if not ok then return false, tostring(key) end
    local sig; ok, sig = pcall(M.parse_signature, sig_blob)
    if not ok then return false, tostring(sig) end

    alg = alg or sig.algorithm
    local a = M.ALGORITHMS[alg]
    if not a or sig.algorithm ~= alg then
        return false, "ssh: host key signature is " .. wire.safe_name(sig.algorithm)
                      .. ", negotiated " .. wire.safe_name(tostring(alg))
    end
    if key.type ~= a.key_type then
        return false, "ssh: a " .. wire.safe_name(key.type)
                      .. " key cannot sign under " .. alg
    end

    local good
    if alg == M.ALGORITHM then
        good = crypto.ed25519_verify(h, sig.signature, key.key)
    else
        local s = sig.signature
        if key.n then
            -- RFC 8332 section 3: the signature is as long as the modulus.
            -- Some servers drop leading zero bytes; pad them back, since the
            -- verifier takes exactly the modulus length.
            if #s > #key.n then
                return false, "ssh: RSA signature is longer than the modulus"
            end
            s = string.rep("\0", #key.n - #s) .. s
        end
        good = crypto.verify(a.jose, M.public_key_pem(key), h, s)
    end
    if good then return true end
    -- The signature is over the exchange hash, which binds the host key to
    -- THIS exchange. A failure here means the peer does not hold the private
    -- half of the key it presented.
    return false, "ssh: host key signature does not verify"
end

-- Trust ---------------------------------------------------------------------

M.TRUSTED = "trusted"   -- stored key matches the one presented
M.UNKNOWN = "unknown"   -- no stored key for this host
M.CHANGED = "changed"   -- a key is stored and it is NOT this one
M.REVOKED = "revoked"   -- the store revokes this key (known_hosts @revoked)

-- The name a host's key is stored under: OpenSSH's known_hosts convention.
-- Lower-cased, because DNS names are case-insensitive and "Web1" and "web1"
-- are one machine; and "[host]:port" off port 22, because two sshds on one
-- machine (22 and 2222, say) have different keys, and filing both under the
-- bare name reported the second as a changed key - or, worse, trusted a key
-- accepted for one port on the other.
function M.store_name(host, port)
    local h = tostring(host):lower()
    -- A host name is one known_hosts pattern. A comma separates patterns,
    -- `*` / `?` / `!` are wildcards and negation, `|` starts a hashed entry,
    -- `#` a comment, `[` `]` the port form built below: one of these in the
    -- name (from an app-chosen host under a wildcard grant) wrote an entry
    -- that also trusted the key for ANOTHER host - "victim.org,x.example.com".
    if h == "" or h:find("[,#|*?!%[%]%s%c]") then
        error("ssh.hostkey: invalid host name for the trust store", 2)
    end
    port = port or 22
    if port == 22 then return h end
    return "[" .. h .. "]:" .. tostring(port)
end

-- Compare the presented key against the store. Makes no decision: returns the
-- status so the caller can apply its own policy to UNKNOWN, and see CHANGED
-- for what it is. `host` is a store_name.
function M.check(store, host, key_blob)
    if type(store) ~= "table" or type(store.get) ~= "function" then
        error("ssh.hostkey: a store with a get function is required", 2)
    end
    -- Revocation first: a revoked key is refused even if some line trusts it.
    -- `revoked` is optional; only stores that can revoke (file_store) have it.
    if type(store.revoked) == "function" and store.revoked(key_blob) then
        return M.REVOKED, nil
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
function M.verify(crypto, store, host, key_blob, sig_blob, h, alg)
    local ok, reason = M.verify_signature(crypto, key_blob, sig_blob, h, alg)
    if not ok then
        return { ok = false, reason = reason }
    end

    local status, stored = M.check(store, host, key_blob)
    local out = {
        ok = true,
        status = status,
        fingerprint = M.fingerprint(crypto.sha256, key_blob),
        key_blob = key_blob,
    }
    if status == M.CHANGED then
        -- Both fingerprints, because the useful question for whoever is
        -- reading is which key they were expecting.
        out.stored_fingerprint = M.fingerprint(crypto.sha256, stored)
    end
    return out
end

return M
