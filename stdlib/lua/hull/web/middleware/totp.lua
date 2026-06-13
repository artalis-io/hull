--- Time-based One-Time Password (RFC 6238) middleware.
--
-- @module hull.web.middleware.totp
-- @license AGPL-3.0-or-later
--
-- ## What this module provides
--
-- A standalone 2FA layer that composes with the existing
-- `hull/web/middleware/auth` + `hull/web/middleware/session`
-- modules. After password verify, the session is marked
-- `pending_2fa = true`; the middleware here gates sensitive routes
-- until a valid TOTP (or recovery) code is presented.
--
--   GET / POST /2fa            -- enrollment form + verification form
--                              -- (the APP owns these routes; this
--                              -- module only provides the verify
--                              -- primitives + the middleware that
--                              -- redirects to them)
--   /private/* (anywhere)      -- gated by totp.middleware() until
--                              -- pending_2fa is cleared
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
--     app.use("*", "/private/*", totp.middleware({
--         redirect_path = "/2fa",
--         skip_paths = { "/2fa", "/logout" },
--     }))
--
--     -- Enrollment + verification routes (app owns these):
--     app.post("/2fa/enroll", function(req, res)
--         local r = totp.enroll(req.ctx.session.user_id)
--         res:html(render_qr_page(r.qr_svg, r.recovery_codes))
--     end)
--     app.post("/2fa/confirm", function(req, res)
--         if totp.confirm(req.ctx.session.user_id, req.form.code) then
--             req.ctx.session.pending_2fa = false
--             res:redirect("/")
--         else
--             res:status(400):html("invalid code")
--         end
--     end)
--     app.post("/2fa/verify", function(req, res)
--         local ok, kind = totp.verify(req.ctx.session.user_id,
--                                       req.form.code)
--         if ok then
--             req.ctx.session.pending_2fa = false
--             res:redirect("/")
--         end
--     end)

local crypto = require("hull.crypto")
local db     = require("hull.db")
local time   = require("hull.time")
local qrcode = require("hull.qrcode")

local totp = {}

-- ── Module state ───────────────────────────────────────────────────

local _state = {
    issuer             = "Hull",
    digits             = 6,
    period             = 30,
    window             = 1,
    recovery_codes     = 10,
    encryption_key     = nil,  -- raw 32 bytes when set
    encryption_key_hex = nil,  -- pre-encoded for crypto.secretbox
    _initialized       = false,
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
-- high rate. RFC 6238 §4 calls this out explicitly. Pair with
-- account lockout (hull/web/middleware/auth_lockout, separate
-- module) for defense in depth.
local function ct_eq(a, b)
    if type(a) ~= "string" or type(b) ~= "string" then return false end
    if #a ~= #b then return false end
    local diff = 0
    for i = 1, #a do
        diff = diff | (string.byte(a, i) ~ string.byte(b, i))
    end
    return diff == 0
end

-- At-rest encryption: nonce(24) || ct. NaCl secretbox auth-checks
-- on open; corrupted ciphertext → nil. The nonce is fresh per
-- encryption, so the same secret bytes encrypted twice produce
-- different blobs (no oracle on enrollment status).
local SECRETBOX_NONCE_LEN = 24
local SECRETBOX_MAC_LEN   = 16

local function encrypt_secret(secret_bytes)
    if not _state.encryption_key_hex then return secret_bytes, 0 end
    local nonce = crypto.random(SECRETBOX_NONCE_LEN)
    local nonce_hex = bytes_to_hex(nonce)
    local ct_hex = crypto.secretbox(secret_bytes, nonce_hex,
                                     _state.encryption_key_hex)
    -- Wire format: 24 raw nonce bytes + ciphertext (hex-decoded).
    return nonce .. hex_to_bytes(ct_hex), 1
end

local function decrypt_secret(blob, encrypted)
    if encrypted == 0 then return blob end
    if not _state.encryption_key_hex then return nil end
    if #blob < SECRETBOX_NONCE_LEN + SECRETBOX_MAC_LEN then return nil end
    local nonce = blob:sub(1, SECRETBOX_NONCE_LEN)
    local ct    = blob:sub(SECRETBOX_NONCE_LEN + 1)
    local nonce_hex = bytes_to_hex(nonce)
    local ct_hex = bytes_to_hex(ct)
    local pt = crypto.secretbox_open(ct_hex, nonce_hex,
                                      _state.encryption_key_hex)
    return pt  -- nil on tamper / wrong key
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
    local secret = decrypt_secret(row.secret, row.encrypted)
    if not secret then return nil end  -- decrypt failure → treat as missing
    return {
        secret         = secret,
        confirmed      = row.confirmed,
        digits         = row.digits,
        period         = row.period,
        last_used_step = row.last_used_step,
    }
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
--   * `encryption_key` Optional 32-byte string. When set, secrets
--                      are NaCl-secretbox-encrypted at rest. Caller
--                      manages the key (env var, fs.read of a
--                      manifest-allowlisted file, etc.).
function totp.init(opts)
    opts = opts or {}
    if opts.digits and opts.digits ~= 6 and opts.digits ~= 8 then
        error("totp.init: digits must be 6 or 8")
    end
    if opts.encryption_key and type(opts.encryption_key) ~= "string" then
        error("totp.init: encryption_key must be a string")
    end
    if opts.encryption_key and #opts.encryption_key ~= 32 then
        error("totp.init: encryption_key must be exactly 32 bytes")
    end
    _state.issuer         = opts.issuer         or _state.issuer
    _state.digits         = opts.digits         or _state.digits
    _state.period         = opts.period         or _state.period
    _state.window         = opts.window         or _state.window
    _state.recovery_codes = opts.recovery_codes or _state.recovery_codes
    _state.encryption_key = opts.encryption_key
    _state.encryption_key_hex = opts.encryption_key
        and bytes_to_hex(opts.encryption_key)
        or nil

    -- Idempotent table creation. db.batch wraps the multi-statement
    -- DDL in a transaction so a crash mid-init leaves a consistent
    -- DB.
    db.batch(function()
        for stmt in SCHEMA:gmatch("([^;]+);") do
            local s = stmt:gsub("^%s+", ""):gsub("%s+$", "")
            if #s > 0 then db.exec(s) end
        end
    end)

    _state._initialized = true
end

--- Enroll a user. Generates a new secret + recovery codes; the row
-- is marked unconfirmed until @ref totp.confirm completes. Calling
-- enroll on an already-enrolled user OVERWRITES (intentional —
-- re-enrollment is the recovery path when a user loses both the
-- authenticator and the recovery codes).
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
        -- Wipe any prior enrollment (and its recovery codes).
        db.exec("DELETE FROM _hull_totp WHERE user_id = ?", { user_id })
        db.exec("DELETE FROM _hull_totp_recovery WHERE user_id = ?",
                { user_id })
        db.exec(
            "INSERT INTO _hull_totp "
            .. "(user_id, secret, encrypted, confirmed, digits, period, "
            .. " last_used_step, created_at, updated_at) "
            .. "VALUES (?, ?, ?, 0, ?, ?, -1, ?, ?)",
            { user_id, stored, encrypted_flag,
              _state.digits, _state.period, now, now })
        for i = 1, #hashes do
            db.exec(
                "INSERT INTO _hull_totp_recovery (user_id, code_hash) "
                .. "VALUES (?, ?)", { user_id, hashes[i] })
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
    local row = load_secret(user_id)
    if not row then return false end
    if row.confirmed == 1 then return true end  -- already confirmed

    local now_step = current_step()
    for offset = -_state.window, _state.window do
        local step = now_step + offset
        if ct_eq(totp_at_step(row.secret, step, row.digits), code) then
            db.batch(function()
                db.exec(
                    "UPDATE _hull_totp SET confirmed = 1, "
                    .. "last_used_step = ?, updated_at = ? "
                    .. "WHERE user_id = ?",
                    { step, time.now(), user_id })
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
function totp.verify(user_id, code)
    check_initialized()
    if type(user_id) ~= "string" or type(code) ~= "string" then
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
            return true, "recovery"
        end
    end

    return false, nil
end

--- Delete a user's secret + every recovery code. Use on account
-- deletion or user-initiated 2FA disable. Returns true if a row
-- was actually removed, false otherwise.
function totp.disable(user_id)
    check_initialized()
    if type(user_id) ~= "string" then return false end
    local removed = 0
    db.batch(function()
        removed = db.exec("DELETE FROM _hull_totp WHERE user_id = ?",
                          { user_id })
        db.exec("DELETE FROM _hull_totp_recovery WHERE user_id = ?",
                { user_id })
    end)
    return removed > 0
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

--- Mint a middleware function. Redirects to `opts.redirect_path`
-- whenever the current session has the pending-2FA flag set.
function totp.middleware(opts)
    check_initialized()
    opts = opts or {}
    local redirect_path = opts.redirect_path or "/2fa"
    local session_key   = opts.session_key   or "pending_2fa"
    local skip_paths    = opts.skip_paths    or { "/2fa", "/logout" }
    -- Convert skip_paths to a set for O(1) lookup at request time.
    local skip = {}
    for i = 1, #skip_paths do skip[skip_paths[i]] = true end

    return function(req, res)
        if skip[req.path] then return 0 end
        local sess = req.ctx and req.ctx.session
        if not sess or not sess[session_key] then return 0 end
        res:redirect(redirect_path)
        return 1
    end
end

-- ── Test helpers (not public; exposed for unit tests) ──────────────
--
-- The TOTP / HOTP digest and the Base32 codec are pure functions
-- with well-known RFC vectors. Test-only access lets the unit tests
-- pin them against those vectors without exporting the surface.
totp._test = {
    base32_encode           = base32_encode,
    base32_decode           = base32_decode,
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
    reset = function()
        _state.issuer             = "Hull"
        _state.digits             = 6
        _state.period             = 30
        _state.window             = 1
        _state.recovery_codes     = 10
        _state.encryption_key     = nil
        _state.encryption_key_hex = nil
        _state._initialized       = false
    end,
}

return totp
