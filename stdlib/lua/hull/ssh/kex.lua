-- hull.ssh.kex - the curve25519-sha256 key exchange.
--
-- RFC 8731 for the exchange, RFC 4253 section 7.2 for key derivation.
--
-- Crypto is PASSED IN rather than required here. Partly so the ordering rules
-- below can be tested without a hash implementation, but mostly because the
-- two things this file gets wrong silently are both orderings: the exchange
-- hash is a bare concatenation with no framing of its own, and the KDF chains
-- one digest into the next. A transposed field in either produces keys that
-- are wrong in a way nothing reports - the connection simply fails to
-- decrypt, or worse, agrees on something an attacker predicted. So the
-- byte-sequence builders are pure functions returning exactly what gets
-- hashed, and the tests assert those bytes.

local wire = require('hull.ssh.wire')

local M = {}

M.SSH_MSG_NEWKEYS        = 21
M.SSH_MSG_KEX_ECDH_INIT  = 30
M.SSH_MSG_KEX_ECDH_REPLY = 31

-- X25519 public values are always 32 bytes (RFC 7748).
M.POINT_LEN = 32

-- Messages ------------------------------------------------------------

-- SSH_MSG_KEX_ECDH_INIT: our ephemeral public value.
function M.build_ecdh_init(q_c)
    if type(q_c) ~= "string" or #q_c ~= M.POINT_LEN then
        error("ssh.kex: Q_C must be exactly 32 bytes", 2)
    end
    return wire.writer()
        :byte(M.SSH_MSG_KEX_ECDH_INIT)
        :string(q_c)
        :build()
end

-- SSH_MSG_KEX_ECDH_REPLY: host key blob, the peer ephemeral value, and the
-- signature over the exchange hash.
function M.parse_ecdh_reply(payload)
    local r = wire.reader(payload)
    local msg = r:byte()
    if msg ~= M.SSH_MSG_KEX_ECDH_REPLY then
        error("ssh.kex: expected KEX_ECDH_REPLY (31), got " .. tostring(msg))
    end
    local host_key  = r:string()
    local q_s       = r:string()
    local signature = r:string()
    if #q_s ~= M.POINT_LEN then
        -- A short or long point would otherwise reach X25519, which accepts
        -- any 32 bytes and would silently use a truncated value.
        error("ssh.kex: server Q_S is " .. tostring(#q_s) .. " bytes, expected 32")
    end
    return { host_key = host_key, q_s = q_s, signature = signature }
end

-- Exchange hash --------------------------------------------------------

-- The exact byte sequence hashed to produce H (RFC 8731 section 3):
--
--   H = hash(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K)
--
-- Every field except K is length-prefixed as a `string`; K is an `mpint`.
-- That asymmetry is not decoration - K is a number and the others are byte
-- strings, and encoding K as a string would produce a different hash from
-- every other implementation.
--
-- V_C and V_S are the identification lines WITHOUT their CR LF.
-- I_C and I_S are the full KEXINIT payloads, message byte included.
function M.exchange_hash_input(f)
    for _, name in ipairs({ "v_c", "v_s", "i_c", "i_s", "k_s", "q_c", "q_s", "k" }) do
        if type(f[name]) ~= "string" then
            error("ssh.kex: exchange hash field " .. name .. " is missing", 2)
        end
    end
    return wire.writer()
        :string(f.v_c)
        :string(f.v_s)
        :string(f.i_c)
        :string(f.i_s)
        :string(f.k_s)
        :string(f.q_c)
        :string(f.q_s)
        :mpint(f.k)
        :build()
end

-- The signed blob for host key verification.
--
-- The server signs H itself, so this is only a name for that fact: the
-- signature in KEX_ECDH_REPLY is over the exchange hash and nothing else.
function M.signed_data(h)
    return h
end

-- Key derivation --------------------------------------------------------

-- RFC 4253 section 7.2. Six keys come out of the same shared secret, each
-- tagged by one letter:
--
--   A  initial IV, client to server      D  encryption key, server to client
--   B  initial IV, server to client      E  integrity key, client to server
--   C  encryption key, client to server  F  integrity key, server to client
--
-- Direction is what keeps the two halves of the connection from sharing key
-- material: a client that derived the same key for both directions would let
-- a peer replay our own ciphertext back at us.
M.KEY_IDS = { iv_c2s = "A", iv_s2c = "B", key_c2s = "C",
              key_s2c = "D", mac_c2s = "E", mac_s2c = "F" }

-- Derive one key of `want` bytes.
--
-- K1 = HASH(K || H || X || session_id)
-- K2 = HASH(K || H || K1), K3 = HASH(K || H || K1 || K2), ...
-- key = K1 || K2 || ... truncated to `want`
--
-- K is mpint-encoded and H and session_id are raw. Mixing that up is the
-- classic way to produce a KDF that works against itself and nothing else.
function M.derive_key(hash, k_mpint, h, session_id, id, want)
    if type(hash) ~= "function" then
        error("ssh.kex: derive_key needs a hash function", 2)
    end
    if #id ~= 1 then
        error("ssh.kex: key id must be a single letter", 2)
    end
    local prefix = k_mpint .. h
    local out = hash(prefix .. id .. session_id)
    -- Each round hashes EVERY byte produced so far, not just the last digest.
    while #out < want do
        out = out .. hash(prefix .. out)
    end
    return out:sub(1, want)
end

-- Derive the whole key set for a negotiated cipher.
--
-- `sizes` gives the byte counts the chosen algorithms need; a MAC size of 0
-- (the AEAD case) skips that key rather than deriving one nobody uses.
function M.derive_keys(hash, k_raw, h, session_id, sizes)
    -- K enters the KDF as an mpint, so it is encoded ONCE here and reused,
    -- rather than being re-encoded per key where a caller could get it wrong.
    local k_mpint = wire.writer():mpint(k_raw):build()

    local out = {}
    for field, id in pairs(M.KEY_IDS) do
        local want = sizes[field]
        if want and want > 0 then
            out[field] = M.derive_key(hash, k_mpint, h, session_id, id, want)
        end
    end
    return out
end

-- Byte counts for the algorithms Hull negotiates. aes256-gcm carries its own
-- authentication, so the integrity keys are zero: deriving them would imply a
-- MAC that is never applied.
M.SIZES = {
    ["aes256-gcm@openssh.com"] = {
        iv_c2s = 12, iv_s2c = 12,
        key_c2s = 32, key_s2c = 32,
        mac_c2s = 0,  mac_s2c = 0,
    },
}

-- Hex bridging -----------------------------------------------------------
--
-- SSH is defined over raw bytes; hull.crypto takes and returns hex (sha256,
-- x25519 and the rest all do). That mismatch has to be crossed somewhere, and
-- doing it here once is better than at each of the half-dozen call sites the
-- transport will have, where a single missed conversion produces key material
-- that is wrong without being obviously wrong.

function M.to_hex(raw)
    return (raw:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

function M.from_hex(hex)
    if #hex % 2 ~= 0 or hex:find("[^0-9a-fA-F]") then
        error("ssh.kex: not a hex string", 2)
    end
    return (hex:gsub("%x%x", function(cc)
        return string.char(tonumber(cc, 16))
    end))
end

-- Wrap a hex-returning hash as the raw-bytes hash derive_key expects.
function M.raw_hash(hash_hex)
    return function(data) return M.from_hex(hash_hex(data)) end
end

return M
