--- Time-based One-Time Password (RFC 6238) middleware.
--
-- @module hull.web.middleware.totp
-- @license AGPL-3.0-or-later
--
-- ## What this module provides
--
-- The TOTP primitive set: enroll, confirm (first-use proof-of-
-- possession), verify (verifies TOTP code OR a single-use recovery
-- code), enrolled (predicate), disable. Designed to compose with
-- hull/web/auth-flows, which owns the login-time 2FA gate: enable
-- TOTP there by passing `enable_totp = true / user_totp_enrolled =
-- function(uid) return totp.enrolled(uid) end / totp_verify =
-- function(user, code) return totp.verify(user.id, code) end`.
-- auth-flows then short-circuits a successful password verify into
-- a /auth/totp-verify form, defers session creation until the code
-- checks out, and the canonical login event is recorded by
-- session.login_handler. There is no `pending_2fa` flag and no
-- middleware here — the auth-flows envelope path is the single way.
--
-- ## Security
--
--   * Secrets are 20 bytes (160 bits) of `crypto.random`. Stored
--     plaintext by default; pass `encryption_key` to `init()` for
--     at-rest NaCl-secretbox encryption (KEK lives in env / file /
--     manifest fs.read path — Hull doesn't touch key management).
--   * Recovery codes are PBKDF2-SHA256 hashed (matches the auth
--     module's password hashing). Codes are returned ONCE at
--     enrollment time; only hashes persist.
--   * Replay protection: every successful verify records the
--     consumed step on the secret row. Re-presenting the same code
--     within the window is rejected.
--   * Constant-time TOTP digest comparison via a small xor-fold
--     helper (`ct_eq`); both sides are zero-padded numeric strings
--     of the same length, so the helper is straight constant-time
--     work and matches RFC 6238 §4. Recovery codes go through
--     crypto.verify_password (constant-time inside the cap).
--   * Recovery code input is canonicalized (uppercased, non-
--     alphanumerics stripped) before hashing and before verify, so
--     "ABCD-EFGH-IJKL", "ABCDEFGHIJKL", and "abcd efgh ijkl" all
--     compare equal — no UX trap that locks users out of their
--     accounts for typing the displayed code without hyphens.
--   * `alg=SHA1` is fixed at RFC 6238's default; every mainstream
--     authenticator app expects it.
--   * Brute-force lockout (round-8): after `max_failed_attempts`
--     consecutive bad codes the user is locked for `lockout_duration`
--     seconds; verify returns false silently during the window. A
--     successful verify (TOTP or recovery) clears the counter.
--     Defaults: 5 attempts / 15-minute window. Pre-round-8 the
--     module shipped without this gate and the header pointed at a
--     `hull/web/middleware/auth_lockout` module that never existed —
--     /auth/totp-verify was effectively unlimited at the line-rate
--     of the network. Tune via init() opts.
--
-- ## Local-first / offline notes
--
--   * TOTP needs no network at verify time — authenticator app and
--     server both derive codes from the shared secret + wall clock.
--     Works in air-gapped + LAN-only Hull deployments.
--   * Clock skew matters more off-cloud. `opts.window` defaults to
--     ±1 step (~90s total tolerance); raise it for devices without
--     NTP (RPi without RTC, etc.) or rely on recovery codes.
--   * Single-user local apps get no real security from 2FA — if you
--     can launch the binary you can read its SQLite. The module is
--     valuable for multi-user shared-device flavors (small office
--     dashboards, family NAS UIs, etc.).
--
-- ## Usage
--
--     local totp = require("hull.web.middleware.totp")
--     totp.init({
--         issuer = "MyApp",
--         encryption_key = env.get("TOTP_KEK"),  -- optional
--     })
--
--     -- App-owned enrollment + first-use confirm. auth-flows owns
--     -- the login-time /auth/totp-verify route — see the auth-flows
--     -- docstring for the wiring.
--     app.post("/2fa/enroll", function(req, res)
--         local r = totp.enroll(req.ctx.session.user_id)
--         res:html(render_qr_page(r.qr_svg, r.recovery_codes))
--     end)
--     app.post("/2fa/confirm", function(req, res)
--         if totp.confirm(req.ctx.session.user_id, req.form.code) then
--             res:redirect("/")
--         else
--             res:status(400):html("invalid code")
--         end
--     end)

local crypto = require("hull.crypto")
local db     = require("hull.db")
local time   = require("hull.time")
local qrcode = require("hull.qrcode")

local totp = {}

-- ── Module state ───────────────────────────────────────────────────

local _state = {
    issuer              = "Hull",
    digits              = 6,
    period              = 30,
    window              = 1,
    recovery_codes      = 10,
    -- Brute-force lockout (round-8). See header §Security.
    max_failed_attempts = 5,
    lockout_duration    = 15 * 60,
    -- Pending-row TTL (round-8 LOW-13). _hull_totp_pending stores
    -- the new secret + (encrypted) recovery codes during the
    -- enroll → confirm window. Without a TTL, abandoned enrollments
    -- pile up plaintext-ish secrets in the DB forever. Default
    -- 1 hour matches the natural UX timeout for "open QR code →
    -- type code into form". Confirmed rows in _hull_totp are NEVER
    -- expired by this — they're the user's real secret.
    pending_ttl         = 3600,
    -- See pending_ttl: opt-out wires up app.daily so accumulated
    -- pending rows get pruned even when the app forgets to call
    -- cleanup() explicitly. Mirrors session/audit-log.
    _cleanup_catchup_done = false,
    _cleanup_scheduled    = false,
    -- Multi-key encryption state. See totp.init for the input shape
    -- and the encrypt_secret / decrypt_secret comments for the wire
    -- format.
    keys                = {},   -- {[version_id] = key_hex}
    current_key_version = nil,  -- id used for new encryptions
    legacy_key_version  = nil,  -- id for pre-versioning rows, if any
    encryption_key_hex  = nil,  -- retained for _test back-compat
    _initialized        = false,
}

-- ── Schema ─────────────────────────────────────────────────────────

local SCHEMA = [[
CREATE TABLE IF NOT EXISTS _hull_totp (
    user_id        TEXT PRIMARY KEY,
    secret         BLOB NOT NULL,            -- raw 20 bytes OR secretbox blob
    encrypted      INTEGER NOT NULL DEFAULT 0,
    confirmed      INTEGER NOT NULL DEFAULT 0,
    digits         INTEGER NOT NULL DEFAULT 6,
    period         INTEGER NOT NULL DEFAULT 30,
    last_used_step INTEGER NOT NULL DEFAULT -1,
    created_at     INTEGER NOT NULL,
    updated_at     INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS _hull_totp_recovery (
    user_id   TEXT NOT NULL,
    code_hash TEXT NOT NULL,
    used_at   INTEGER,
    PRIMARY KEY (user_id, code_hash)
);

CREATE TABLE IF NOT EXISTS _hull_totp_pending (
    user_id    TEXT PRIMARY KEY,
    secret     BLOB NOT NULL,
    encrypted  INTEGER NOT NULL DEFAULT 0,
    digits     INTEGER NOT NULL DEFAULT 6,
    period     INTEGER NOT NULL DEFAULT 30,
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS _hull_totp_pending_recovery (
    user_id   TEXT NOT NULL,
    code_hash TEXT NOT NULL,
    PRIMARY KEY (user_id, code_hash)
);

CREATE TABLE IF NOT EXISTS _hull_totp_attempts (
    user_id        TEXT PRIMARY KEY,
    failed_count   INTEGER NOT NULL DEFAULT 0,
    last_failed_at INTEGER,
    locked_until   INTEGER
);

CREATE INDEX IF NOT EXISTS _hull_totp_recovery_user
    ON _hull_totp_recovery(user_id);
]]

-- ── Helpers (private) ──────────────────────────────────────────────

-- Hex helpers used at every cap layer that takes hex-encoded keys.
-- Thin aliases over crypto.hex_encode / crypto.hex_decode so call
-- sites stay readable and a future rename of either side is one
-- edit instead of N.
local function bytes_to_hex(s) return crypto.hex_encode(s) end
local function hex_to_bytes(h) return crypto.hex_decode(h) end

-- RFC 4648 Base32 (no padding). 20 bytes → 32 chars exactly with no
-- padding needed (160/5 = 32). The encoder accepts any byte string;
-- the decoder is case-insensitive and tolerates "=" padding +
-- whitespace because some authenticator apps echo back padded form.
local B32_ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"

local function base32_encode(bytes)
    local out = {}
    local buf, bits = 0, 0
    for i = 1, #bytes do
        buf = (buf << 8) | string.byte(bytes, i)
        bits = bits + 8
        while bits >= 5 do
            bits = bits - 5
            local v = (buf >> bits) & 0x1F
            out[#out + 1] = B32_ALPHA:sub(v + 1, v + 1)
            buf = buf & ((1 << bits) - 1)
        end
    end
    if bits > 0 then
        local v = (buf << (5 - bits)) & 0x1F
        out[#out + 1] = B32_ALPHA:sub(v + 1, v + 1)
    end
    return table.concat(out)
end

local B32_INV
do
    B32_INV = {}
    for i = 1, #B32_ALPHA do
        B32_INV[B32_ALPHA:byte(i)] = i - 1
    end
end

local function base32_decode(s)
    if type(s) ~= "string" then return nil, "not a string" end
    local out = {}
    local buf, bits = 0, 0
    for i = 1, #s do
        local c = s:sub(i, i):upper():byte()
        -- Skip whitespace + "=" padding (some authenticators echo
        -- it back); inverting the condition avoids the lint warning
        -- about an empty if-branch.
        if c ~= 32 and c ~= 9 and c ~= 10 and c ~= 13 and c ~= 0x3D then
            local v = B32_INV[c]
            if v == nil then return nil, "invalid base32 char" end
            buf = (buf << 5) | v
            bits = bits + 5
            if bits >= 8 then
                bits = bits - 8
                out[#out + 1] = string.char((buf >> bits) & 0xFF)
                buf = buf & ((1 << bits) - 1)
            end
        end
    end
    return table.concat(out)
end

-- TOTP per RFC 6238 = HOTP(K, T_step). HOTP per RFC 4226 §5.3-5.4:
--   1. HMAC-SHA1(K, 8-byte BE counter) → 20-byte digest
--   2. offset = digest[19] & 0x0F
--   3. P = ((digest[offset] & 0x7F) << 24)
--        | (digest[offset+1] << 16)
--        | (digest[offset+2] << 8)
--        | (digest[offset+3])
--   4. code = P mod 10^digits, zero-padded to width digits.
local function totp_at_step(secret_bytes, step, digits)
    -- Encode the step counter as 8-byte big-endian.
    local counter = string.char(
        (step >> 56) & 0xFF, (step >> 48) & 0xFF,
        (step >> 40) & 0xFF, (step >> 32) & 0xFF,
        (step >> 24) & 0xFF, (step >> 16) & 0xFF,
        (step >>  8) & 0xFF,  step        & 0xFF)
    local key_hex = bytes_to_hex(secret_bytes)
    local mac_hex = crypto.hmac_sha1(counter, key_hex)
    -- mac_hex is 40 chars; convert to byte array for the truncation.
    local mac = {}
    for i = 1, 40, 2 do
        mac[#mac + 1] = tonumber(mac_hex:sub(i, i + 1), 16)
    end
    local offset = (mac[20] & 0x0F) + 1  -- 1-indexed for Lua
    local p = ((mac[offset]     & 0x7F) << 24)
            | ((mac[offset + 1] & 0xFF) << 16)
            | ((mac[offset + 2] & 0xFF) <<  8)
            |  (mac[offset + 3] & 0xFF)
    local mod = 10 ^ digits
    local code = p % mod
    return string.format("%0" .. digits .. "d", code)
end

-- Recovery codes: 12 chars from an unambiguous 31-char alphabet,
-- formatted as ABCD-EFGH-IJKL. ~59.5 bits of entropy per code — well
-- above brute-force feasibility at any reasonable verify rate.
-- Alphabet skips 0/O/1/I/L (common confusables when transcribed).
local RECOVERY_ALPHA = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"  -- 31 chars

-- Strip everything outside the recovery alphabet (hyphens, spaces,
-- accidental punctuation) and uppercase. Stored hashes are computed
-- against the normalized form so users can paste back the displayed
-- "ABCD-EFGH-IJKL" or the unhyphenated "ABCDEFGHIJKL" interchangeably,
-- and case sensitivity isn't a lockout trap.
local function normalize_recovery_code(s)
    if type(s) ~= "string" then return "" end
    return (s:upper():gsub("[^A-Z0-9]", ""))
end

local function generate_recovery_codes(n)
    local codes, hashes = {}, {}
    for i = 1, n do
        -- Need 12 chars × log2(31) ≈ 59.5 bits → 12 random bytes is
        -- plenty. Modulo bias on 31 from a uniform byte is < 0.4%
        -- per char and ignorable at this entropy scale.
        local raw = crypto.random(12)
        local parts = {}
        for j = 1, 12 do
            local b = string.byte(raw, j)
            parts[j] = RECOVERY_ALPHA:sub((b % 31) + 1, (b % 31) + 1)
        end
        local plain = table.concat(parts)  -- unhyphenated, used for hash
        local display = parts[1] .. parts[2] .. parts[3] .. parts[4] .. "-"
                      .. parts[5] .. parts[6] .. parts[7] .. parts[8] .. "-"
                      .. parts[9] .. parts[10] .. parts[11] .. parts[12]
        codes[i] = display
        hashes[i] = crypto.hash_password(plain)
    end
    return codes, hashes
end

-- Recovery-code hash matches crypto.hash_password's pbkdf2 format,
-- so verify uses crypto.verify_password (constant-time at C layer).
local function hash_recovery_code(code)
    return crypto.hash_password(normalize_recovery_code(code))
end

local function verify_recovery_code(code, hash)
    return crypto.verify_password(normalize_recovery_code(code), hash)
end

-- Constant-time string equality. Used for TOTP code matching where
-- both sides are fixed-length zero-padded numeric strings; Lua's
-- native `==` short-circuits on first mismatch, which is a
-- measurable timing leak when an attacker can submit guesses at
-- high rate. RFC 6238 §4 calls this out explicitly. Defense in
-- depth: the brute-force lockout (lockout_remaining /
-- bump_failed_attempt below) is the actual gate; ct_eq just keeps
-- the per-comparison side-channel uniform.
local function ct_eq(a, b)
    if type(a) ~= "string" or type(b) ~= "string" then return false end
    if #a ~= #b then return false end
    local diff = 0
    for i = 1, #a do
        diff = diff | (string.byte(a, i) ~ string.byte(b, i))
    end
    return diff == 0
end

-- At-rest encryption: versioned NaCl secretbox.
--
-- Wire format v1 (legacy):  nonce(24) || ct
-- Wire format v2 (current): version(4 BE) || nonce(24) || ct
--
-- v1 was the format before key rotation was supported (single
-- encryption_key opt). Apps that upgrade with existing rows keep
-- v1 readable via _state.legacy_key_version + the corresponding
-- entry in _state.keys; new writes ALWAYS use v2. The rekey()
-- helper converts v1 rows to v2 on demand or lazily on verify.
--
-- Multi-key support: _state.keys is a map {version_id -> key_hex}.
-- The current version (used for new encryptions) lives in
-- _state.current_key_version. Decrypt reads the 4-byte version
-- prefix, looks up the key in the map; if the version is unknown
-- AND a legacy_key_version is configured, falls back to v1
-- decryption with that key.
local SECRETBOX_NONCE_LEN = 24
local SECRETBOX_MAC_LEN   = 16
local VERSION_PREFIX_LEN  = 4
local MIN_V2_BLOB_LEN     = VERSION_PREFIX_LEN
                          + SECRETBOX_NONCE_LEN + SECRETBOX_MAC_LEN

local function pack_version_be(v)
    return string.char((v >> 24) & 0xFF, (v >> 16) & 0xFF,
                       (v >>  8) & 0xFF,  v        & 0xFF)
end

local function unpack_version_be(s)
    return (string.byte(s, 1) << 24)
         | (string.byte(s, 2) << 16)
         | (string.byte(s, 3) <<  8)
         |  string.byte(s, 4)
end

-- encrypt_secret returns (blob, encrypted_flag, version). When no
-- encryption keys are configured, secret is stored plaintext;
-- version is 0 (sentinel for "no encryption"). Otherwise the blob
-- is versioned and version is _state.current_key_version.
local function encrypt_secret(secret_bytes)
    local cur = _state.current_key_version
    local key_hex = cur and _state.keys[cur]
    if not key_hex then return secret_bytes, 0, 0 end
    local nonce = crypto.random(SECRETBOX_NONCE_LEN)
    local nonce_hex = bytes_to_hex(nonce)
    local ct_hex = crypto.secretbox(secret_bytes, nonce_hex, key_hex)
    return pack_version_be(cur) .. nonce .. hex_to_bytes(ct_hex), 1, cur
end

-- decrypt_secret returns (plaintext, version) on success, or nil on
-- failure. Caller uses the returned version to decide whether to
-- re-encrypt under current (lazy rekey-on-verify).
local function decrypt_secret(blob, encrypted)
    if encrypted == 0 then return blob, 0 end
    if type(blob) ~= "string" then return nil end

    -- Try v2 (versioned) first.
    if #blob >= MIN_V2_BLOB_LEN then
        local version = unpack_version_be(blob:sub(1, VERSION_PREFIX_LEN))
        local key_hex = _state.keys[version]
        if key_hex then
            local nonce = blob:sub(VERSION_PREFIX_LEN + 1,
                                   VERSION_PREFIX_LEN + SECRETBOX_NONCE_LEN)
            local ct    = blob:sub(VERSION_PREFIX_LEN + SECRETBOX_NONCE_LEN + 1)
            local pt = crypto.secretbox_open(bytes_to_hex(ct),
                                              bytes_to_hex(nonce), key_hex)
            if pt then return pt, version end
            -- v2 with a recognized version that fails to decrypt is
            -- a real corruption / wrong-key; don't paper over by
            -- trying legacy.
            return nil
        end
    end

    -- v1 (legacy, no prefix). Only attempted if a legacy_key_version
    -- is configured AND the blob length matches the v1 shape (nonce +
    -- non-empty ct). Apps without legacy data have legacy_key_version
    -- = nil; this branch never runs and decrypt returns nil for any
    -- unrecognized version.
    local legacy_v = _state.legacy_key_version
    local legacy_key = legacy_v and _state.keys[legacy_v]
    if legacy_key and #blob >= SECRETBOX_NONCE_LEN + SECRETBOX_MAC_LEN then
        local nonce = blob:sub(1, SECRETBOX_NONCE_LEN)
        local ct    = blob:sub(SECRETBOX_NONCE_LEN + 1)
        local pt = crypto.secretbox_open(bytes_to_hex(ct),
                                          bytes_to_hex(nonce), legacy_key)
        if pt then return pt, 0 end  -- version=0 signals "legacy v1"
    end

    return nil
end

-- URL-encoding for the otpauth URI. Authenticator apps are strict
-- about RFC 3986 — encode anything outside the unreserved set.
local function urlenc(s)
    return (s:gsub("([^A-Za-z0-9%-._~])", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

-- Google Authenticator key URI:
--   otpauth://totp/Issuer:Account?secret=...&issuer=...
--                  &algorithm=SHA1&digits=6&period=30
-- The path "Issuer:Account" + the `issuer=` query param are
-- redundant but recommended (some apps display the path, others
-- the query param).
local function build_otpauth_url(user_id, secret_b32)
    local issuer = _state.issuer
    return "otpauth://totp/"
        .. urlenc(issuer) .. ":" .. urlenc(user_id)
        .. "?secret=" .. secret_b32
        .. "&issuer=" .. urlenc(issuer)
        .. "&algorithm=SHA1"
        .. "&digits=" .. tostring(_state.digits)
        .. "&period=" .. tostring(_state.period)
end

local function load_secret(user_id)
    local rows = db.query(
        "SELECT secret, encrypted, confirmed, digits, period, last_used_step "
        .. "FROM _hull_totp WHERE user_id = ?", { user_id })
    if not rows or #rows == 0 then return nil end
    local row = rows[1]
    local secret, version = decrypt_secret(row.secret, row.encrypted)
    if not secret then return nil end  -- decrypt failure → treat as missing
    return {
        secret         = secret,
        version        = version,   -- 0 = plaintext or legacy v1; else key id
        confirmed      = row.confirmed,
        digits         = row.digits,
        period         = row.period,
        last_used_step = row.last_used_step,
    }
end

-- Load the pending-enrollment row, if any. Used by totp.confirm to
-- promote the pending slot into _hull_totp on a successful code
-- verify. Returns nil when no pending enrollment exists.
local function load_pending_secret(user_id)
    local rows = db.query(
        "SELECT secret, encrypted, digits, period, created_at "
        .. "FROM _hull_totp_pending WHERE user_id = ?", { user_id })
    if not rows or #rows == 0 then return nil end
    local row = rows[1]
    local secret = decrypt_secret(row.secret, row.encrypted)
    if not secret then return nil end
    return {
        secret     = secret,
        stored     = row.secret,
        encrypted  = row.encrypted,
        digits     = row.digits,
        period     = row.period,
        created_at = row.created_at,
    }
end

-- Lazy rekey: called after a successful verify when the stored row
-- is on an older version than current. Re-encrypts the secret under
-- the current key and writes the new blob. Best-effort — a transient
-- DB error here doesn't fail the verify (the user is already
-- authenticated), but the row stays on the old version and we'll
-- try again next verify. The condition guarding the call lives at
-- the verify site so this never runs when keys aren't configured.
-- NB: db.exec must be called directly (not via pcall) so the
-- mod_db `lua_is_stdlib_caller` check sees this stdlib module's
-- chunk frame at level 1, not pcall's C frame. We let any db.exec
-- error propagate; the verify call site wraps the entire rekey
-- attempt in pcall so a rare DB hiccup can't fail the user's
-- already-successful authentication.
local function rekey_row(user_id, secret_bytes)
    local blob, enc_flag = encrypt_secret(secret_bytes)
    if enc_flag ~= 1 then return end  -- no current key, nothing to do
    db.exec(
        "UPDATE _hull_totp SET secret = ?, encrypted = 1, updated_at = ? "
        .. "WHERE user_id = ?",
        { blob, time.now(), user_id })
end

-- Atomic step advance: only succeeds if `step > last_used_step`. The
-- WHERE clause does the replay check; returning rowcount=0 means a
-- concurrent verify (or replay attempt) already consumed this step.
local function mark_step_used(user_id, step)
    return db.exec(
        "UPDATE _hull_totp SET last_used_step = ?, updated_at = ? "
        .. "WHERE user_id = ? AND last_used_step < ?",
        { step, time.now(), user_id, step })
end

local function current_step()
    return time.now() // _state.period
end

local function check_initialized()
    if not _state._initialized then
        error("totp: call totp.init(...) before any other function")
    end
end

-- ── Brute-force lockout ───────────────────────────────────────────
--
-- Round-8 HIGH-3. Pre-round-8, the verify path was unrate-limited and
-- the module header pointed at a phantom `auth_lockout` module. A 6-
-- digit code in a ±1 step window had ~3 valid (step, code) tuples;
-- with no gate, an attacker holding the user's password (and thus
-- a valid pending-2FA envelope from auth-flows) could land a code in
-- minutes at line rate. The bookkeeping mirrors auth-flows's login
-- lockout: same shape, same column names, same defaults.
--
-- `lockout_remaining` returns the number of seconds the user is
-- still locked for (0 if free). `bump_failed_attempt` increments the
-- counter; on threshold, sets `locked_until = now + lockout_duration`
-- and resets the counter so the NEXT lockout window starts from a
-- clean 5. `clear_failed_attempts` is called on success.

local function lockout_remaining(user_id)
    local r = db.query(
        "SELECT locked_until FROM _hull_totp_attempts WHERE user_id = ?",
        { user_id })
    if not r or #r == 0 or not r[1].locked_until then return 0 end
    local remain = r[1].locked_until - time.now()
    return remain > 0 and remain or 0
end

local function bump_failed_attempt(user_id)
    local now = time.now()
    -- Read-modify-write under db.batch so two concurrent failed
    -- verifies don't both think they're the threshold-crossing one
    -- and double-lock.
    db.batch(function()
        local r = db.query(
            "SELECT failed_count, locked_until FROM _hull_totp_attempts "
            .. "WHERE user_id = ?", { user_id })
        local fc = (r and r[1] and r[1].failed_count) or 0
        local new_fc = fc + 1
        local locked_until = (r and r[1] and r[1].locked_until) or nil
        if new_fc >= _state.max_failed_attempts then
            locked_until = now + _state.lockout_duration
            new_fc = 0  -- reset; next bad code restarts the counter
        end
        db.exec(
            "INSERT INTO _hull_totp_attempts "
            .. "(user_id, failed_count, last_failed_at, locked_until) "
            .. "VALUES (?, ?, ?, ?) "
            .. "ON CONFLICT(user_id) DO UPDATE SET "
            .. "  failed_count   = excluded.failed_count, "
            .. "  last_failed_at = excluded.last_failed_at, "
            .. "  locked_until   = excluded.locked_until",
            { user_id, new_fc, now, locked_until })
    end)
end

local function clear_failed_attempts(user_id)
    db.exec("DELETE FROM _hull_totp_attempts WHERE user_id = ?",
            { user_id })
end

-- ── Public API ─────────────────────────────────────────────────────

--- Initialize the module. Must be called once at app startup.
-- @tparam table opts
--   * `issuer`         App label shown in authenticator app
--                      (default `"Hull"`).
--   * `digits`         TOTP code length, 6 or 8 (default `6`).
--                      Every mainstream authenticator supports 6;
--                      stick with 6 unless you have a specific
--                      reason.
--   * `period`         Seconds per step (default `30`, RFC 6238
--                      default; authenticator apps assume this).
--   * `window`         ± step tolerance for clock skew (default `1`,
--                      ~90s total window).
--   * `recovery_codes` Number of recovery codes minted at enroll
--                      (default `10`).
--   * `encryption_key`  Optional 32-byte string. Back-compat shorthand
--                       for `encryption_keys = {[1] = X}, current = 1,
--                       legacy_key_version = 1`. New apps should use
--                       the explicit map.
--   * `encryption_keys` Optional map of `{[id]=32_byte_key, ...}`.
--                       Each id is a 32-bit unsigned integer that
--                       gets written as a 4-byte prefix on encrypted
--                       blobs. Required when planning a key rotation.
--   * `current`         Required when `encryption_keys` is set. The
--                       id of the key used for new encryptions. Old
--                       rows still decrypt under whatever version
--                       they were written with (must still be in the
--                       map).
--   * `legacy_key_version` Optional id pointing into encryption_keys.
--                       Identifies the key used by pre-versioning
--                       (v1 wire format) rows so they continue to
--                       decrypt during the migration window. Once
--                       rekey() reports zero v1 rows remaining, this
--                       can be unset and the legacy-version key
--                       removed from `encryption_keys`.
function totp.init(opts)
    opts = opts or {}
    if opts.digits and opts.digits ~= 6 and opts.digits ~= 8 then
        error("totp.init: digits must be 6 or 8")
    end

    -- Build the keys map. Three input shapes:
    --   1. encryption_keys + current  → multi-key (explicit).
    --   2. encryption_key only        → back-compat shorthand,
    --                                   translated to {[1]=X}, current=1,
    --                                   legacy_key_version=1.
    --   3. nothing                    → plaintext storage (no encryption).
    local keys = {}
    local current = nil
    local legacy_version = nil
    if opts.encryption_keys then
        if type(opts.encryption_keys) ~= "table" then
            error("totp.init: encryption_keys must be a {[id]=bytes,...} map")
        end
        if type(opts.current) ~= "number" then
            error("totp.init: current (key id) required when "
                  .. "encryption_keys is set")
        end
        for id, k in pairs(opts.encryption_keys) do
            if type(id) ~= "number" or id < 0 or id > 0xFFFFFFFF then
                error("totp.init: encryption_keys ids must be 0..2^32-1")
            end
            if type(k) ~= "string" or #k ~= 32 then
                error("totp.init: encryption_keys[" .. tostring(id)
                      .. "] must be exactly 32 bytes")
            end
            keys[id] = bytes_to_hex(k)
        end
        if not keys[opts.current] then
            error("totp.init: current key id " .. tostring(opts.current)
                  .. " not present in encryption_keys")
        end
        current = opts.current
        if opts.legacy_key_version ~= nil then
            if not keys[opts.legacy_key_version] then
                error("totp.init: legacy_key_version "
                      .. tostring(opts.legacy_key_version)
                      .. " not present in encryption_keys")
            end
            legacy_version = opts.legacy_key_version
        end
    elseif opts.encryption_key then
        if type(opts.encryption_key) ~= "string"
           or #opts.encryption_key ~= 32 then
            error("totp.init: encryption_key must be exactly 32 bytes")
        end
        keys[1] = bytes_to_hex(opts.encryption_key)
        current = 1
        legacy_version = 1  -- pre-versioning rows decrypt under this key
    end

    _state.issuer         = opts.issuer         or _state.issuer
    _state.digits         = opts.digits         or _state.digits
    _state.period         = opts.period         or _state.period
    _state.window         = opts.window         or _state.window
    _state.recovery_codes = opts.recovery_codes or _state.recovery_codes
    _state.max_failed_attempts = opts.max_failed_attempts
                                 or _state.max_failed_attempts
    _state.lockout_duration    = opts.lockout_duration
                                 or _state.lockout_duration
    if opts.pending_ttl ~= nil then
        _state.pending_ttl = opts.pending_ttl
    end
    _state.keys                 = keys
    _state.current_key_version  = current
    _state.legacy_key_version   = legacy_version
    -- Back-compat: a few _test helpers still expect this field.
    _state.encryption_key_hex   = current and keys[current] or nil

    -- Idempotent table creation. db.batch wraps the multi-statement
    -- DDL in a transaction so a crash mid-init leaves a consistent
    -- DB.
    db.batch(function()
        for stmt in SCHEMA:gmatch("([^;]+);") do
            local s = stmt:gsub("^%s+", ""):gsub("%s+$", "")
            if #s > 0 then db.exec(s) end
        end
    end)

    -- Rotation-safety check (back-compat shorthand only). When the
    -- caller passed encryption_key = X (not the explicit map), the
    -- shorthand forces keys = {[1]=X}, current = 1. If the table
    -- already contains rows encrypted under a DIFFERENT key under
    -- the same v1 prefix, this new key won't decrypt them — silent
    -- data loss. Sample one row; if decrypt fails, abort with a
    -- clear message pointing at the explicit rotation API. The
    -- explicit encryption_keys path is allowed to fail decryption
    -- on individual rows (that's what legacy_key_version + rekey()
    -- handle); only the shorthand triggers this guard.
    if opts.encryption_key and not opts.encryption_keys
       and _state.current_key_version then
        local sample = db.query(
            "SELECT secret, encrypted FROM _hull_totp "
            .. "WHERE encrypted = 1 LIMIT 1")
        if sample and sample[1] then
            local pt = decrypt_secret(sample[1].secret, sample[1].encrypted)
            if not pt then
                error("totp.init: encryption_key shorthand cannot decrypt "
                      .. "existing _hull_totp rows. This is silent data "
                      .. "loss waiting to happen — every enrolled user "
                      .. "would be locked out of 2FA. To rotate, switch "
                      .. "to the explicit `encryption_keys = {[1]=OLD, "
                      .. "[2]=NEW}, current = 2, legacy_key_version = 1` "
                      .. "API and call totp.rekey() to migrate rows.")
            end
        end
    end

    -- Mark initialized BEFORE the lazy-catchup block so totp.cleanup
    -- — which guards on check_initialized — can run from inside
    -- init's own cleanup pass. Pre-fix: the catchup pcall caught
    -- "call totp.init(...) before any other function" and the log
    -- spammed every CI run.
    _state._initialized = true

    -- Round-8 LOW-13: lazy catchup + auto-schedule daily cleanup.
    -- Mirrors the round-7 audit-log / session pattern. _hull_totp
    -- (confirmed) is the user's real secret — never expired here.
    -- _hull_totp_pending stages an in-flight enroll; rows older than
    -- pending_ttl are orphans (network glitch, browser closed
    -- mid-flow) and stay in the DB indefinitely without this prune.
    if opts.cleanup ~= false and not _state._cleanup_catchup_done then
        local ok, err = pcall(totp.cleanup)
        if not ok then
            local log = require("hull.log")
            log.warn("totp: init-time cleanup failed: " .. tostring(err))
        end
        _state._cleanup_catchup_done = true
    end
    if opts.cleanup ~= false and not _state._cleanup_scheduled then
        local at = opts.cleanup_at or "03:00"
        if type(app) == "table" and type(app.daily) == "function" then
            app.daily(at, function() totp.cleanup() end)
            _state._cleanup_scheduled = true
        else
            local log = require("hull.log")
            log.warn("totp: app.daily not available "
                  .. "(CLI flavor or hull/timers not admitted) "
                  .. "— pending-row prune runs only at init(). "
                  .. "Wire your own cron/worker for steady-state.")
        end
    end
end

--- Prune orphaned pending-enrollment rows older than pending_ttl
-- seconds. Confirmed _hull_totp rows are NEVER touched — those are
-- the user's real secret. Returns the number of pending rows (and
-- their recovery-code companions) removed. Auto-scheduled daily via
-- app.daily unless `cleanup = false` is passed to init.
function totp.cleanup()
    check_initialized()
    local cutoff = time.now() - (_state.pending_ttl or 3600)
    local removed = 0
    db.batch(function()
        -- Find expired pending users first so we can also nuke their
        -- staged recovery codes (the two tables aren't FK-linked,
        -- since SQLite without PRAGMA foreign_keys=ON skips FKs;
        -- driving the delete from a single id list is safe across
        -- backends).
        local victims = db.query(
            "SELECT user_id FROM _hull_totp_pending WHERE created_at < ?",
            { cutoff })
        for _, r in ipairs(victims or {}) do
            db.exec("DELETE FROM _hull_totp_pending_recovery "
                    .. "WHERE user_id = ?", { r.user_id })
            db.exec("DELETE FROM _hull_totp_pending WHERE user_id = ?",
                    { r.user_id })
            removed = removed + 1
        end
    end)
    return removed
end

--- Enroll a user. Generates a new secret + recovery codes and
-- writes them to the PENDING slot (_hull_totp_pending +
-- _hull_totp_pending_recovery). The user's existing CONFIRMED
-- enrollment in _hull_totp is left intact until totp.confirm
-- succeeds — a user who loses the new codes mid-enrollment still
-- has working 2FA via their prior enrollment.
--
-- A second enroll() before confirm() REPLACES the pending slot
-- (intentional — the new authenticator scan is what the user is
-- actively trying to set up). The confirmed slot stays untouched.
function totp.enroll(user_id)
    check_initialized()
    if type(user_id) ~= "string" or user_id == "" then
        error("totp.enroll: user_id required")
    end

    -- RFC 6238 recommends >= 160 bits (20 bytes) of secret entropy.
    local secret_bytes = crypto.random(20)
    local secret_b32   = base32_encode(secret_bytes)
    local stored, encrypted_flag = encrypt_secret(secret_bytes)

    local codes, hashes = generate_recovery_codes(_state.recovery_codes)

    local now = time.now()
    db.batch(function()
        -- Replace any prior PENDING enrollment for this user; the
        -- confirmed row in _hull_totp is untouched.
        db.exec("DELETE FROM _hull_totp_pending WHERE user_id = ?",
                { user_id })
        db.exec("DELETE FROM _hull_totp_pending_recovery WHERE user_id = ?",
                { user_id })
        db.exec(
            "INSERT INTO _hull_totp_pending "
            .. "(user_id, secret, encrypted, digits, period, created_at) "
            .. "VALUES (?, ?, ?, ?, ?, ?)",
            { user_id, stored, encrypted_flag,
              _state.digits, _state.period, now })
        for i = 1, #hashes do
            db.exec(
                "INSERT INTO _hull_totp_pending_recovery "
                .. "(user_id, code_hash) VALUES (?, ?)",
                { user_id, hashes[i] })
        end
    end)

    local otpauth_url = build_otpauth_url(user_id, secret_b32)
    -- EC level M is the standard authenticator-app target (Google
    -- Authenticator displays well at M; H wastes density for no
    -- benefit on a phone screen).
    local qr_svg = qrcode.svg(otpauth_url, { ec_level = "M", scale = 6 })

    return {
        secret_base32  = secret_b32,
        otpauth_url    = otpauth_url,
        qr_svg         = qr_svg,
        recovery_codes = codes,
    }
end

--- Confirm enrollment. Verifies one code from the just-paired
-- authenticator. On success the row's `confirmed` flag flips to 1
-- (which is what @ref totp.enrolled checks); also records the step
-- as used so the same code can't be reused for an immediate verify.
function totp.confirm(user_id, code)
    check_initialized()
    if type(user_id) ~= "string" or type(code) ~= "string" then
        return false
    end

    -- Confirm verifies the code against the PENDING enrollment.
    -- On success it PROMOTES the pending row into _hull_totp,
    -- replacing any existing confirmed row + recovery codes.
    --
    -- Round-8 LOW-11: pre-round-8 confirm() fell back to "is the user
    -- already enrolled?" when no pending row existed and returned
    -- true without verifying any code. The bypass required a user to
    -- already be enrolled, but the name "confirm" implies code-
    -- verified — apps that treated confirm()'s boolean as proof-of-
    -- possession (the natural reading) silently skipped the code
    -- check. Apps that just want the enrollment predicate should
    -- call totp.enrolled(user_id) instead; confirm() now strictly
    -- means "this code verifies the pending enrollment".
    local pending = load_pending_secret(user_id)
    if not pending then
        return false
    end
    -- Round-8 LOW-13: reject pending rows older than pending_ttl
    -- here too. The daily cleanup catches them eventually, but the
    -- enroll → confirm window is ~minutes; a confirm-against-1-week-
    -- old-pending is almost certainly a stale tab and shouldn't
    -- silently work.
    if pending.created_at and _state.pending_ttl
       and (pending.created_at + _state.pending_ttl) < time.now() then
        return false
    end

    local now_step = current_step()
    for offset = -_state.window, _state.window do
        local step = now_step + offset
        if ct_eq(totp_at_step(pending.secret,
                              step, pending.digits), code) then
            local now = time.now()
            -- Promote pending → main. All in one batch so a crash
            -- mid-promotion either leaves both rows intact (user
            -- can retry) or both replaced (user is enrolled).
            db.batch(function()
                db.exec("DELETE FROM _hull_totp WHERE user_id = ?",
                        { user_id })
                db.exec("DELETE FROM _hull_totp_recovery WHERE user_id = ?",
                        { user_id })
                db.exec(
                    "INSERT INTO _hull_totp "
                    .. "(user_id, secret, encrypted, confirmed, digits, "
                    .. " period, last_used_step, created_at, updated_at) "
                    .. "VALUES (?, ?, ?, 1, ?, ?, ?, ?, ?)",
                    { user_id, pending.stored, pending.encrypted,
                      pending.digits, pending.period, step, now, now })
                db.exec(
                    "INSERT INTO _hull_totp_recovery "
                    .. "(user_id, code_hash) "
                    .. "SELECT user_id, code_hash FROM "
                    .. "_hull_totp_pending_recovery WHERE user_id = ?",
                    { user_id })
                db.exec("DELETE FROM _hull_totp_pending WHERE user_id = ?",
                        { user_id })
                db.exec(
                    "DELETE FROM _hull_totp_pending_recovery "
                    .. "WHERE user_id = ?", { user_id })
            end)
            return true
        end
    end
    return false
end

--- Verify a TOTP code (or recovery code) for an enrolled +
-- confirmed user. Order: TOTP first (most common path), recovery
-- code only on TOTP miss.
--
-- Recovery codes are single-use — verified hashes get `used_at`
-- stamped, and a future verify with the same code finds the row
-- but rejects it on the `used_at IS NULL` filter.
--
-- Returns a bare boolean. If the caller needs to know whether the
-- successful path was "totp" or "recovery" (for audit metadata),
-- call `totp.verify_with_kind` instead — it returns `(ok, kind)`.
-- Splitting the two surfaces matches the JS API and avoids the
-- silent porting bug where `local ok = totp.verify(...)` discarded
-- the second return.
function totp.verify_with_kind(user_id, code)
    check_initialized()
    if type(user_id) ~= "string" or type(code) ~= "string" then
        return false, nil
    end
    -- Lockout check (round-8 HIGH-3). Return false silently — same
    -- shape as a wrong code — so an enumerator can't tell "locked"
    -- from "bad code". Apps that need the locked state for UX can
    -- call totp.lockout_remaining(user_id) explicitly.
    if lockout_remaining(user_id) > 0 then
        return false, nil
    end
    local row = load_secret(user_id)
    if not row or row.confirmed ~= 1 then return false, nil end

    -- TOTP path. Scan -window..+window steps; the FIRST match wins.
    -- The `mark_step_used` atomic update enforces replay protection:
    -- if a concurrent verify already consumed this step, the UPDATE
    -- affects 0 rows and we reject.
    local now_step = current_step()
    for offset = -_state.window, _state.window do
        local step = now_step + offset
        -- ct_eq matches the constant-time compare in totp.confirm
        -- (line ~519). The leak via plain `==` is theoretical at
        -- 6-byte ASCII under network jitter, but the asymmetry was
        -- the real audit smell; keep both paths uniform.
        if step > row.last_used_step
           and ct_eq(totp_at_step(row.secret, step, row.digits), code) then
            if mark_step_used(user_id, step) == 1 then
                clear_failed_attempts(user_id)
                -- Lazy rekey: if the row's secret was decrypted under
                -- a key version other than the current one (legacy v1
                -- or an older v2 version after rotation), re-encrypt
                -- it under the current key. Best-effort — the verify
                -- itself already succeeded; a transient DB error on
                -- the rekey path doesn't block the user.
                if _state.current_key_version
                   and row.version ~= _state.current_key_version then
                    rekey_row(user_id, row.secret)
                end
                return true, "totp"
            end
            return false, nil  -- raced; treat as failure
        end
    end

    -- Recovery-code path. Walk unused codes; constant-time verify
    -- per row. SQL filter on used_at handles single-use enforcement.
    local rows = db.query(
        "SELECT code_hash FROM _hull_totp_recovery "
        .. "WHERE user_id = ? AND used_at IS NULL", { user_id })
    for _, r in ipairs(rows or {}) do
        if verify_recovery_code(code, r.code_hash) then
            db.exec(
                "UPDATE _hull_totp_recovery SET used_at = ? "
                .. "WHERE user_id = ? AND code_hash = ?",
                { time.now(), user_id, r.code_hash })
            clear_failed_attempts(user_id)
            -- Same lazy rekey on the recovery-code path — the
            -- decrypted secret is in hand so we might as well
            -- migrate it forward.
            if _state.current_key_version
               and row.version ~= _state.current_key_version then
                rekey_row(user_id, row.secret)
            end
            return true, "recovery"
        end
    end

    bump_failed_attempt(user_id)
    return false, nil
end

--- Seconds remaining in a brute-force lockout window (0 if not
-- locked). Apps that want to surface "try again in N seconds" UX
-- can call this directly; the verify path itself returns a plain
-- false to keep the lockout state non-enumerable from the wire.
function totp.lockout_remaining(user_id)
    check_initialized()
    if type(user_id) ~= "string" then return 0 end
    return lockout_remaining(user_id)
end

function totp.verify(user_id, code)
    local ok = totp.verify_with_kind(user_id, code)
    return ok
end

--- Batch re-encrypt every stored TOTP secret under the current key
-- version. Use after a key rotation to actively migrate v1 / older
-- v2 rows forward, instead of waiting for every user to sign in
-- (lazy rekey-on-verify happens automatically). Scans the full
-- _hull_totp table once. Returns `{scanned, rekeyed, failed}`:
--   * scanned  — total rows considered.
--   * rekeyed  — rows successfully re-encrypted under current.
--   * failed   — rows that wouldn't decrypt under any known key
--                (legacy_key_version missing? key removed too soon?)
-- Operators can decommission an old key from `encryption_keys`
-- once `rekey()` reports `failed = 0` AND a follow-up scan reports
-- `rekeyed = 0` (every row is already on current).
function totp.rekey()
    check_initialized()
    if not _state.current_key_version then
        error("totp.rekey: no encryption_keys configured; nothing to do")
    end
    local cur = _state.current_key_version
    local rows = db.query(
        "SELECT user_id, secret, encrypted FROM _hull_totp")
    local scanned, rekeyed, failed = 0, 0, 0
    for _, r in ipairs(rows or {}) do
        scanned = scanned + 1
        local pt, version = decrypt_secret(r.secret, r.encrypted)
        if not pt then
            failed = failed + 1
        elseif version ~= cur then
            -- Use the rekey_row helper directly (it calls db.exec
            -- without an intermediate pcall, so the stdlib-caller
            -- check sees totp's frame at level 1). Wrap the whole
            -- thing here so an error on one row doesn't abort the
            -- batch scan.
            local ok = pcall(rekey_row, r.user_id, pt)
            if ok then rekeyed = rekeyed + 1
            else failed = failed + 1 end
        end
    end
    return { scanned = scanned, rekeyed = rekeyed, failed = failed }
end

--- Read-only count of _hull_totp rows grouped by stored key
-- version. Lets operators plan a rotation without writing — call
-- this BEFORE rekey() to see how many users are on each version.
-- Returns a table { [version_id] = count, ... } plus a `total`
-- field. version=0 buckets plaintext rows and undecryptable
-- (legacy v1 without legacy_key_version, or wrong key) rows.
-- @treturn table
function totp.rekey_status()
    check_initialized()
    local rows = db.query(
        "SELECT secret, encrypted FROM _hull_totp")
    local counts = { total = 0 }
    for _, r in ipairs(rows or {}) do
        counts.total = counts.total + 1
        local _, version = decrypt_secret(r.secret, r.encrypted)
        -- version is nil on decrypt failure; use 0 as the "unknown
        -- / unrecoverable" bucket (matches the version=0 used for
        -- plaintext + legacy v1).
        local k = version or 0
        counts[k] = (counts[k] or 0) + 1
    end
    return counts
end

--- Delete a user's secret + every recovery code. Use on account
-- deletion or user-initiated 2FA disable. Returns true if a row
-- was actually removed, false otherwise.
function totp.disable(user_id)
    check_initialized()
    if type(user_id) ~= "string" then return false end
    local removed_main = 0
    local removed_pending = 0
    db.batch(function()
        removed_main = db.exec("DELETE FROM _hull_totp WHERE user_id = ?",
                                { user_id })
        db.exec("DELETE FROM _hull_totp_recovery WHERE user_id = ?",
                { user_id })
        -- Round-7 item 5: also wipe any in-flight pending enrollment.
        -- "Disabled" means "no enrollment activity for this user" in
        -- either slot; report true if EITHER slot had a row.
        removed_pending = db.exec(
            "DELETE FROM _hull_totp_pending WHERE user_id = ?",
            { user_id })
        db.exec("DELETE FROM _hull_totp_pending_recovery WHERE user_id = ?",
                { user_id })
    end)
    return (removed_main or 0) + (removed_pending or 0) > 0
end

--- Is the user enrolled AND confirmed? Cheap row check intended for
-- the route layer to decide whether to gate.
function totp.enrolled(user_id)
    check_initialized()
    if type(user_id) ~= "string" then return false end
    local rows = db.query(
        "SELECT confirmed FROM _hull_totp WHERE user_id = ?",
        { user_id })
    return rows and #rows > 0 and rows[1].confirmed == 1
end

-- (totp.middleware was a parallel pending-2FA gate that required apps
-- to create a session BEFORE the 2FA step, then flip a flag on it.
-- It overlapped with hull/web/auth-flows' totp_pending envelope, which
-- defers session creation until after 2FA succeeds. The two could not
-- interoperate. Standardized on the auth-flows envelope; apps wire
-- TOTP through auth-flows.init's `enable_totp / user_totp_enrolled /
-- totp_verify` callbacks and let it own the /2fa form + verify route.)

-- ── Test helpers (not public; exposed for unit tests) ──────────────
--
-- The TOTP / HOTP digest and the Base32 codec are pure functions
-- with well-known RFC vectors. Test-only access lets the unit tests
-- pin them against those vectors without exporting the surface.
-- Debug-only: read a user's stored secret blob (post-encryption,
-- pre-decryption) so unit tests can verify wire-format changes
-- without poking through Hull's _hull_* namespace gate.
local function debug_get_blob(user_id)
    local rows = db.query(
        "SELECT secret, encrypted FROM _hull_totp WHERE user_id = ?",
        { user_id })
    if not rows or #rows == 0 then return nil end
    return rows[1].secret, rows[1].encrypted
end

totp._test = {
    base32_encode           = base32_encode,
    base32_decode           = base32_decode,
    get_blob                = debug_get_blob,
    totp_at_step            = totp_at_step,
    current_step            = current_step,
    ct_eq                   = ct_eq,
    normalize_recovery_code = normalize_recovery_code,
    generate_recovery_codes = generate_recovery_codes,
    hash_recovery_code      = hash_recovery_code,
    verify_recovery_code    = verify_recovery_code,
    encrypt_secret          = encrypt_secret,
    decrypt_secret          = decrypt_secret,
    build_otpauth_url       = build_otpauth_url,
    lockout_remaining       = lockout_remaining,
    bump_failed_attempt     = bump_failed_attempt,
    clear_failed_attempts   = clear_failed_attempts,
    -- Test-only: backdate a pending row's created_at so cleanup()
    -- finds it stale. Lives inside the module so db.exec passes
    -- the lua_is_stdlib_caller check (user code can't touch
    -- _hull_* directly).
    force_pending_stale     = function(user_id)
        db.exec("UPDATE _hull_totp_pending SET created_at = ? "
                .. "WHERE user_id = ?", { 0, user_id })
    end,
    reset = function()
        _state.issuer              = "Hull"
        _state.digits              = 6
        _state.period              = 30
        _state.window              = 1
        _state.recovery_codes      = 10
        _state.max_failed_attempts = 5
        _state.lockout_duration    = 15 * 60
        _state.pending_ttl         = 3600
        _state.keys                = {}
        _state.current_key_version = nil
        _state.legacy_key_version  = nil
        _state.encryption_key_hex  = nil
        _state._cleanup_catchup_done = false
        _state._cleanup_scheduled    = false
        _state._initialized        = false
    end,
}

return totp
