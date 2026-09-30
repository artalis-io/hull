/**
 * @file hull:web:middleware:totp
 * @module hull:web:middleware:totp
 * @description Time-based One-Time Password (RFC 6238) middleware.
 *
 * Lua parity: `hull.web.middleware.totp`. Same surface, snake_case
 * keys ↔ camelCase here. See the Lua module header for the security
 * model, threat-model notes, and local-first caveats.
 *
 * @license AGPL-3.0-or-later
 *
 * ## API
 *
 *   totp.init({ issuer, digits, period, window, recoveryCodes,
 *               encryptionKey })
 *   totp.enroll(userId) -> { secretBase32, otpauthUrl, qrSvg, recoveryCodes }
 *   totp.confirm(userId, code) -> bool
 *   totp.verify(userId, code)  -> bool
 *   totp.verifyWithKind(userId, code) -> [ok, "totp" | "recovery" | null]
 *   totp.disable(userId)
 *   totp.enrolled(userId)      -> bool
 *
 * Login-time 2FA gating happens via hull/web/auth-flows (the
 * authFlows.init `enableTotp / userTotpEnrolled / totpVerify`
 * callbacks). There is no `pending_2fa` middleware here; the
 * auth-flows envelope path is the single supported way.
 */

import { crypto } from "hull:crypto";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { time }   from "hull:time";
import { qrcode } from "hull:qrcode";
import { app }    from "hull:app";
import { log }    from "hull:log";
import { _request } from "hull:web:_request";

// ── Module state ───────────────────────────────────────────────────

const _state = {
    issuer:           "Hull",
    digits:           6,
    period:           30,
    window:           1,
    recoveryCodes:    10,
    // Brute-force lockout (round-8). See Lua sibling header.
    maxFailedAttempts:  5,
    lockoutDuration:    15 * 60,
    // Round-9 HIGH-4: per-IP lockout (in addition to per-user). See
    // Lua sibling for the threat model. Looser default (20 vs 5)
    // because shared-NAT / mobile-carrier IPs aggregate users.
    // Round-10 HIGH-4: trustXff default false. Direct-exposed apps
    // would let attackers rotate XFF for fresh buckets per request.
    // Apps behind a trusted reverse proxy set trustXff = true.
    maxFailedAttemptsPerIp: 20,
    lockoutDurationPerIp:   15 * 60,
    trustXff:               false,
    // Pending-row TTL (round-8 LOW-13). See Lua sibling.
    pendingTtl:         3600,
    cleanupCatchupDone: false,
    cleanupScheduled:   false,
    // Multi-key encryption state. See init() + encrypt/decrypt
    // comments for the wire format and the input shape.
    keys:               {},   // {[versionId]: keyHex}
    currentKeyVersion:  null, // id used for new encryptions
    legacyKeyVersion:   null, // id for pre-versioning rows, if any
    initialized:        false,
};

// ── Schema ─────────────────────────────────────────────────────────

const SCHEMA = `
CREATE TABLE IF NOT EXISTS _hull_totp (
    user_id        VARCHAR(255) PRIMARY KEY,
    secret         BLOB NOT NULL,
    encrypted      INTEGER NOT NULL DEFAULT 0,
    confirmed      INTEGER NOT NULL DEFAULT 0,
    digits         INTEGER NOT NULL DEFAULT 6,
    period         INTEGER NOT NULL DEFAULT 30,
    last_used_step INTEGER NOT NULL DEFAULT -1,
    created_at     INTEGER NOT NULL,
    updated_at     INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS _hull_totp_recovery (
    user_id   VARCHAR(255) NOT NULL,
    code_hash VARCHAR(255) NOT NULL,
    used_at   INTEGER,
    PRIMARY KEY (user_id, code_hash)
);

CREATE TABLE IF NOT EXISTS _hull_totp_pending (
    user_id    VARCHAR(255) PRIMARY KEY,
    secret     BLOB NOT NULL,
    encrypted  INTEGER NOT NULL DEFAULT 0,
    digits     INTEGER NOT NULL DEFAULT 6,
    period     INTEGER NOT NULL DEFAULT 30,
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS _hull_totp_pending_recovery (
    user_id   VARCHAR(255) NOT NULL,
    code_hash VARCHAR(255) NOT NULL,
    PRIMARY KEY (user_id, code_hash)
);

CREATE TABLE IF NOT EXISTS _hull_totp_attempts (
    user_id        VARCHAR(255) PRIMARY KEY,
    failed_count   INTEGER NOT NULL DEFAULT 0,
    last_failed_at INTEGER,
    locked_until   INTEGER
);

CREATE TABLE IF NOT EXISTS _hull_totp_attempts_by_ip (
    ip             VARCHAR(255) PRIMARY KEY,
    failed_count   INTEGER NOT NULL DEFAULT 0,
    last_failed_at INTEGER,
    locked_until   INTEGER
);

CREATE INDEX IF NOT EXISTS _hull_totp_recovery_user
    ON _hull_totp_recovery(user_id);

CREATE INDEX IF NOT EXISTS _hull_totp_attempts_lf
    ON _hull_totp_attempts(last_failed_at);

CREATE INDEX IF NOT EXISTS _hull_totp_attempts_by_ip_lf
    ON _hull_totp_attempts_by_ip(last_failed_at);
`;

// ── Helpers ────────────────────────────────────────────────────────

import { encoding } from "hull:encoding";
import { sealbox } from "hull:crypto:sealbox";
import { otp } from "hull:crypto:otp";

// The enrolment secret is shown as RFC 4648 base32, unpadded: 20 bytes are
// exactly 32 characters. Read back leniently, because people and apps copy
// the spaces and '=' padding along with it.
const base32Encode = (bytes) => encoding.base32.encode(bytes);
const base32Decode = (s) => encoding.base32.decode(s, { lenient: true });

// TOTP per RFC 6238 is HOTP(K, T_step); the algorithm lives in
// hull:crypto:otp.
const totpAtStep = otp.hotp;

// Recovery codes: 12 chars from a 31-char no-confusables alphabet,
// formatted as ABCD-EFGH-IJKL. Modulo bias on 31 from a uniform byte
// is negligible at our scale. Returns plaintext + PBKDF2 hashes.
const RECOVERY_ALPHA = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";  // 31 chars

// Strip everything outside the recovery alphabet (hyphens, spaces,
// accidental punctuation) and uppercase. Stored hashes are computed
// against the normalized form so users can paste back "ABCD-EFGH-IJKL",
// "ABCDEFGHIJKL", or "abcd efgh ijkl" interchangeably without a UX
// lockout trap. Matches the Lua side's normalize_recovery_code.
function normalizeRecoveryCode(s) {
    if (typeof s !== "string") return "";
    return s.toUpperCase().replace(/[^A-Z0-9]/g, "");
}

function generateRecoveryCodes(n) {
    const codes = new Array(n);
    const hashes = new Array(n);
    for (let i = 0; i < n; i++) {
        const raw = new Uint8Array(crypto.random(12));
        const parts = new Array(12);
        for (let j = 0; j < 12; j++) parts[j] = RECOVERY_ALPHA[raw[j] % 31];
        const plain = parts.join("");  // unhyphenated, used for hash
        const display = parts[0] + parts[1] + parts[2] + parts[3] + "-"
                      + parts[4] + parts[5] + parts[6] + parts[7] + "-"
                      + parts[8] + parts[9] + parts[10] + parts[11];
        codes[i]  = display;
        hashes[i] = crypto.hashPassword(plain);
    }
    return [codes, hashes];
}

function verifyRecoveryCode(code, hash) {
    return crypto.verifyPassword(normalizeRecoveryCode(code), hash);
}

// Constant-time string equality. TOTP code matching where both
// sides are zero-padded numeric strings of the same length; JS's
// native `===` short-circuits on first code-unit mismatch, a
// measurable timing leak when an attacker can submit guesses at
// high rate. RFC 6238 §4 calls this out. Pair with account lockout
// (hull/web/middleware/auth_lockout, separate module) for defense
// in depth.
function ctEq(a, b) {
    if (typeof a !== "string" || typeof b !== "string") return false;
    // The C constant-time compare jwt / csrf / the Lua side use, rather than
    // a hand-rolled loop the interpreter's timing can leak through.
    return crypto.constantTimeEq(a, b);
}

// At-rest encryption: hull:crypto:sealbox, the versioned secretbox format
// shared with hull:kv's encrypted handles. See the matching Lua module's
// encrypt_secret / decrypt_secret comments for the design - wire format v2
// (current) is a sealbox blob, version(4 BE) || nonce(24) || ct, sealed with
// no context; v1 (legacy, pre-rotation) was nonce(24) || ct. Both runtimes
// produce identical byte sequences for the same key so a Lua<->JS migration
// on the same DB works without re-enrolling.
function keyring() {
    return { keys: _state.keys, current: _state.currentKeyVersion };
}

function encryptSecret(secretBytes) {
    const cur = _state.currentKeyVersion;
    if (cur == null || !_state.keys[cur]) return [secretBytes, 0, 0];
    return [sealbox.seal(keyring(), secretBytes), 1, cur];
}

// Returns [plaintext, version] on success, [null, null] on failure.
// version=0 signals "plaintext storage" OR "legacy v1 row". Callers
// use the version to decide whether to lazily re-encrypt under
// current.
function decryptSecret(blob, encrypted) {
    if (encrypted === 0) return [blob, 0];
    if (typeof blob !== "string") return [null, null];

    const [value, err, version] = sealbox.open(keyring(), blob);
    if (!err) return [value, version];
    // A v2 blob with a recognized version that fails to open is real
    // corruption or a wrong key; don't paper over it with a legacy attempt.
    if (err === "open_failed" && blob.length >= sealbox.MIN_LEN) return [null, null];

    // v1 (legacy) fallback. Only if a legacy_key_version is configured.
    const legacyV = _state.legacyKeyVersion;
    if (legacyV == null) return [null, null];
    const pt = sealbox.openUnversioned(keyring(), legacyV, blob);
    return pt !== null ? [pt, 0] : [null, null];
}

function urlenc(s) {
    return encoding.url.encode(String(s));
}

function buildOtpauthUrl(userId, secretB32) {
    const issuer = _state.issuer;
    return "otpauth://totp/"
        + urlenc(issuer) + ":" + urlenc(userId)
        + "?secret=" + secretB32
        + "&issuer=" + urlenc(issuer)
        + "&algorithm=SHA1"
        + "&digits=" + String(_state.digits)
        + "&period=" + String(_state.period);
}

function loadSecret(userId) {
    const rows = db.query(
        "SELECT secret, encrypted, confirmed, digits, period, last_used_step "
        + "FROM _hull_totp WHERE user_id = ?", [userId]);
    if (!rows || rows.length === 0) return null;
    const row = rows[0];
    // Plain assignment instead of destructuring - QuickJS's
    // js_parse_destructuring_element fails an MSan use-of-uninit
    // check at module-compile time, which fails the MSan + UBSan
    // CI job. Refactored across this module until upstream fixes it.
    const dec = decryptSecret(row.secret, row.encrypted);
    if (!dec[0]) return null;
    return {
        secret:        dec[0],
        version:       dec[1],  // 0 = plaintext/legacy v1; else key id
        confirmed:     row.confirmed,
        digits:        row.digits,
        period:        row.period,
        lastUsedStep:  row.last_used_step,
    };
}

// Load the pending-enrollment row, if any. Used by confirm to
// promote the pending slot into _hull_totp on a successful code
// verify. Returns null when no pending enrollment exists.
function loadPendingSecret(userId) {
    const rows = db.query(
        "SELECT secret, encrypted, digits, period, created_at "
        + "FROM _hull_totp_pending WHERE user_id = ?", [userId]);
    if (!rows || rows.length === 0) return null;
    const row = rows[0];
    const dec = decryptSecret(row.secret, row.encrypted);
    if (!dec[0]) return null;
    return {
        secret:     dec[0],
        stored:     row.secret,
        encrypted:  row.encrypted,
        digits:     row.digits,
        period:     row.period,
        created_at: row.created_at,
    };
}

// Lazy rekey: called after a successful verify when the stored row
// is on an older version than current. Re-encrypts the secret under
// the current key. Best-effort - verify already succeeded; a
// transient DB error here doesn't fail the user.
function rekeyRow(userId, secretBytes) {
    const enc = encryptSecret(secretBytes);
    if (enc[1] !== 1) return;  // no current key, nothing to do
    try {
        db.exec(
            "UPDATE _hull_totp SET secret = ?, encrypted = 1, updated_at = ? "
            + "WHERE user_id = ?",
            [enc[0], time.now(), userId]);
    } catch (_e) { /* best-effort */ }
}

function markStepUsed(userId, step) {
    return db.exec(
        "UPDATE _hull_totp SET last_used_step = ?, updated_at = ? "
        + "WHERE user_id = ? AND last_used_step < ?",
        [step, time.now(), userId, step]);
}

function currentStep() {
    return otp.step(time.now(), _state.period);
}

function checkInitialized() {
    if (!_state.initialized) {
        throw new Error("totp: call totp.init(...) before any other function");
    }
}

// ── Brute-force lockout ───────────────────────────────────────────
// Round-8 HIGH-3. Mirror of stdlib/lua/hull/web/middleware/totp.lua.
// See its header for the threat model.

function lockoutRemaining(userId) {
    const r = db.query(
        "SELECT locked_until FROM _hull_totp_attempts WHERE user_id = ?",
        [userId]);
    if (!r || r.length === 0 || r[0].locked_until == null) return 0;
    const remain = r[0].locked_until - time.now();
    return remain > 0 ? remain : 0;
}

function bumpFailedAttempt(userId) {
    const now = time.now();
    db.batch(() => {
        const r = db.query(
            "SELECT failed_count, locked_until FROM _hull_totp_attempts "
            + "WHERE user_id = ?", [userId]);
        let fc = (r && r[0] && r[0].failed_count) || 0;
        let newFc = fc + 1;
        let lockedUntil = (r && r[0] && r[0].locked_until) || null;
        if (newFc >= _state.maxFailedAttempts) {
            lockedUntil = now + _state.lockoutDuration;
            newFc = 0;
        }
        db.exec(
            "INSERT INTO _hull_totp_attempts "
            + "(user_id, failed_count, last_failed_at, locked_until) "
            + "VALUES (?, ?, ?, ?) "
            + "ON CONFLICT(user_id) DO UPDATE SET "
            + "  failed_count   = excluded.failed_count, "
            + "  last_failed_at = excluded.last_failed_at, "
            + "  locked_until   = excluded.locked_until",
            [userId, newFc, now, lockedUntil]);
    });
}

function clearFailedAttempts(userId) {
    db.exec("DELETE FROM _hull_totp_attempts WHERE user_id = ?",
            [userId]);
}

// Round-9 HIGH-4: per-IP gate. Same shape as the per-user pair.

function lockoutRemainingIp(ip) {
    if (typeof ip !== "string" || ip === "") return 0;
    const r = db.query(
        "SELECT locked_until FROM _hull_totp_attempts_by_ip "
        + "WHERE ip = ?", [ip]);
    if (!r || r.length === 0 || r[0].locked_until == null) return 0;
    const remain = r[0].locked_until - time.now();
    return remain > 0 ? remain : 0;
}

function bumpFailedAttemptIp(ip) {
    if (typeof ip !== "string" || ip === "") return;
    const now = time.now();
    db.batch(() => {
        const r = db.query(
            "SELECT failed_count FROM _hull_totp_attempts_by_ip "
            + "WHERE ip = ?", [ip]);
        const fc = (r && r[0] && r[0].failed_count) || 0;
        let newFc = fc + 1;
        let lockedUntil = null;
        if (newFc >= _state.maxFailedAttemptsPerIp) {
            lockedUntil = now + _state.lockoutDurationPerIp;
            newFc = 0;
        }
        db.exec(
            "INSERT INTO _hull_totp_attempts_by_ip "
            + "(ip, failed_count, last_failed_at, locked_until) "
            + "VALUES (?, ?, ?, ?) "
            + "ON CONFLICT(ip) DO UPDATE SET "
            + "  failed_count   = excluded.failed_count, "
            + "  last_failed_at = excluded.last_failed_at, "
            + "  locked_until   = excluded.locked_until",
            [ip, newFc, now, lockedUntil]);
    });
}

function clearFailedAttemptsIp(ip) {
    if (typeof ip !== "string" || ip === "") return;
    db.exec("DELETE FROM _hull_totp_attempts_by_ip WHERE ip = ?", [ip]);
}

let _xffWarnDone = false;
function extractIp(req) {
    if (!req || typeof req !== "object") return null;
    const xff = (req.headers || {})["x-forwarded-for"];
    if (!_state.trustXff && typeof xff === "string" && xff !== ""
        && !_xffWarnDone) {
        // Round-11 MEDIUM-8: one-shot warn. See Lua sibling.
        _xffWarnDone = true;
        log.warn("totp: X-Forwarded-For seen but trustXff = false; "
            + "per-IP gate is aggregating all requests under the "
            + "proxy's remote_addr. If you're behind a trusted proxy "
            + "that normalizes XFF, set totp.init({trustXff: true}) "
            + "so each upstream client gets its own bucket.");
    }
    // Core extraction (trustXff -> XFF-first -> remote_addr, 64-cap)
    // via the shared hull:web:_request helper; see docs/stdlib_style.md.
    return _request.clientIp(req, _state.trustXff);
}

// ── Public API ─────────────────────────────────────────────────────

/**
 * init opts (encryption-related):
 *   - encryptionKey   Optional 32-byte string. Back-compat shorthand
 *                     for { encryptionKeys: {1: X}, current: 1,
 *                     legacyKeyVersion: 1 }. New apps should use the
 *                     explicit map.
 *   - encryptionKeys  Optional object `{ [id]: 32_byte_key, ... }`.
 *                     Each id is a 32-bit unsigned int that gets
 *                     written as a 4-byte prefix on encrypted blobs.
 *                     Required when planning a key rotation.
 *   - current         Required when encryptionKeys is set. The id of
 *                     the key used for new encryptions.
 *   - legacyKeyVersion Optional id pointing into encryptionKeys.
 *                     Identifies the key used by pre-versioning (v1
 *                     wire format) rows so they continue to decrypt
 *                     during the migration window. Once rekey()
 *                     reports zero v1 rows remaining, unset it and
 *                     remove the legacy-version key from the map.
 */
function init(opts) {
    opts = opts || {};
    if (opts.digits !== undefined && opts.digits !== 6 && opts.digits !== 8) {
        throw new Error("totp.init: digits must be 6 or 8");
    }

    // Build the keys map. Three input shapes:
    //   1. encryptionKeys + current  -> multi-key (explicit)
    //   2. encryptionKey only        -> back-compat shorthand
    //   3. nothing                   -> plaintext storage
    let keys = {};
    let current = null;
    let legacyVersion = null;
    if (opts.encryptionKeys) {
        if (typeof opts.encryptionKeys !== "object") {
            throw new Error("totp.init: encryptionKeys must be a {[id]: bytes} map");
        }
        if (opts.current === undefined || opts.current === null) {
            throw new Error("totp.init: `current` (key id) required when "
                + "encryptionKeys is set");
        }
        // The same keyring rules as every other sealbox user.
        let ring;
        try {
            ring = sealbox.keyring({ keys: opts.encryptionKeys, current: opts.current });
        } catch (e) {
            throw new Error("totp.init: " + String(e && e.message).replace(/^sealbox\.keyring: /, ""));
        }
        keys = ring.keys;
        current = ring.current;
        if (opts.legacyKeyVersion !== undefined && opts.legacyKeyVersion !== null) {
            if (keys[opts.legacyKeyVersion] === undefined) {
                throw new Error("totp.init: legacyKeyVersion "
                    + opts.legacyKeyVersion + " not present in encryptionKeys");
            }
            legacyVersion = opts.legacyKeyVersion;
        }
    } else if (opts.encryptionKey !== undefined && opts.encryptionKey !== null) {
        // 32 bytes, or a crypto.keyFromEnv key; sealbox.keyring decides.
        try {
            keys = sealbox.keyring({ keys: { 1: opts.encryptionKey }, current: 1 }).keys;
        } catch (e) {
            throw new Error("totp.init: encryptionKey "
                            + String(e.message).replace(/^sealbox\.keyring: key 1 /, ""));
        }
        current = 1;
        legacyVersion = 1;  // pre-versioning rows decrypt under this key
    }

    _state.issuer         = opts.issuer         || _state.issuer;
    _state.digits         = opts.digits         || _state.digits;
    _state.period         = opts.period         || _state.period;
    _state.window         = opts.window         !== undefined ? opts.window : _state.window;
    _state.recoveryCodes  = opts.recoveryCodes  || _state.recoveryCodes;
    _state.maxFailedAttempts = opts.maxFailedAttempts
                               || _state.maxFailedAttempts;
    _state.lockoutDuration   = opts.lockoutDuration
                               || _state.lockoutDuration;
    _state.maxFailedAttemptsPerIp = opts.maxFailedAttemptsPerIp
                                    || _state.maxFailedAttemptsPerIp;
    _state.lockoutDurationPerIp   = opts.lockoutDurationPerIp
                                    || _state.lockoutDurationPerIp;
    _state.trustXff               = opts.trustXff === true;
    // Round-12 LOW-2: reset the one-shot XFF warn flag when the
    // caller explicitly touched trustXff. See Lua sibling.
    if (opts.trustXff !== undefined) {
        _xffWarnDone = false;
    }
    // Round-9 LOW-12: <= 0 or `false` → disabled. See Lua sibling.
    if (opts.pendingTtl !== undefined) {
        if (opts.pendingTtl === false) {
            _state.pendingTtl = null;
        } else if (typeof opts.pendingTtl === "number"
                   && opts.pendingTtl <= 0) {
            log.warn("totp.init: pendingTtl <= 0 disables the TTL; "
                + "pass `false` for the explicit opt-out.");
            _state.pendingTtl = null;
        } else {
            _state.pendingTtl = opts.pendingTtl;
        }
    }
    _state.keys                = keys;
    _state.currentKeyVersion   = current;
    _state.legacyKeyVersion    = legacyVersion;

    db.batch(() => {
        const stmts = SCHEMA.split(";");
        for (let i = 0; i < stmts.length; i++) {
            const s = stmts[i].trim();
            if (s.length > 0) db.exec(s);
        }
    });

    // Rotation-safety check (back-compat shorthand only). See the
    // matching Lua comment for the threat model - silent data loss
    // when ops bumps `encryption_key` without using the explicit
    // encryption_keys map for rotation.
    if (opts.encryptionKey != null && opts.encryptionKeys == null
        && _state.currentKeyVersion != null) {
        const sample = db.query(
            "SELECT secret, encrypted FROM _hull_totp "
            + "WHERE encrypted = 1 LIMIT 1");
        if (sample && sample[0]) {
            const dec = decryptSecret(sample[0].secret, sample[0].encrypted);
            if (!dec[0]) {
                throw new Error("totp.init: encryptionKey shorthand cannot "
                    + "decrypt existing _hull_totp rows. This is silent "
                    + "data loss waiting to happen - every enrolled user "
                    + "would be locked out of 2FA. To rotate, switch to "
                    + "the explicit `encryptionKeys: {1: OLD, 2: NEW}, "
                    + "current: 2, legacyKeyVersion: 1` API and call "
                    + "totp.rekey() to migrate rows.");
            }
        }
    }

    // Mark initialized BEFORE the lazy-catchup block so totp.cleanup
    // - which guards on checkInitialized - can run from inside
    // init's own cleanup pass. Pre-fix: the catchup catch swallowed
    // "call totp.init(...) before any other function" on every load.
    _state.initialized = true;

    // Round-8 LOW-13: lazy catchup + auto-schedule daily prune of
    // orphaned _hull_totp_pending rows. Mirrors session/audit-log.
    // _hull_totp (confirmed) is NEVER expired by this.
    if (opts.cleanup !== false && !_state.cleanupCatchupDone) {
        try { totp.cleanup(); }
        catch (e) {
            log.warn("totp: init-time cleanup failed: "
                  + (e && e.message || e));
        }
        _state.cleanupCatchupDone = true;
    }
    if (opts.cleanup !== false && !_state.cleanupScheduled) {
        const at = opts.cleanupAt || "03:00";
        if (app && typeof app.daily === "function") {
            app.daily(at, () => totp.cleanup());
            _state.cleanupScheduled = true;
        } else {
            log.warn("totp: app.daily not available "
                + "(CLI flavor or hull/timers not admitted) "
                + "- pending-row prune runs only at init(). "
                + "Wire your own cron/worker for steady-state.");
        }
    }
}

function cleanup() {
    checkInitialized();
    const now = time.now();
    let removed = 0;
    // Round-10 LOW-11: prune attempts tables too (per-user + per-IP).
    // See Lua sibling for the unbounded-growth rationale.
    if (_state.pendingTtl) {
        const pendingCutoff = now - _state.pendingTtl;
        db.batch(() => {
            const victims = db.query(
                "SELECT user_id FROM _hull_totp_pending "
                + "WHERE created_at < ?", [pendingCutoff]);
            for (let i = 0; i < (victims || []).length; i++) {
                db.exec("DELETE FROM _hull_totp_pending_recovery "
                      + "WHERE user_id = ?", [victims[i].user_id]);
                db.exec("DELETE FROM _hull_totp_pending WHERE user_id = ?",
                        [victims[i].user_id]);
                removed++;
            }
        });
    }
    const attemptsCutoff = now - (_state.lockoutDuration || 0) * 2;
    db.exec(
        "DELETE FROM _hull_totp_attempts "
        + "WHERE (locked_until IS NULL OR locked_until < ?) "
        + "  AND (last_failed_at IS NULL OR last_failed_at < ?)",
        [now, attemptsCutoff]);
    const ipAttemptsCutoff = now
                             - (_state.lockoutDurationPerIp || 0) * 2;
    db.exec(
        "DELETE FROM _hull_totp_attempts_by_ip "
        + "WHERE (locked_until IS NULL OR locked_until < ?) "
        + "  AND (last_failed_at IS NULL OR last_failed_at < ?)",
        [now, ipAttemptsCutoff]);
    return removed;
}

function enroll(userId) {
    checkInitialized();
    if (typeof userId !== "string" || userId === "") {
        throw new Error("totp.enroll: userId required");
    }

    const secretBytes = encoding.bytes.fromBuffer(crypto.random(20));

    const secretB32 = base32Encode(secretBytes);
    const enc = encryptSecret(secretBytes);
    const stored = enc[0];
    const encryptedFlag = enc[1];

    const rc = generateRecoveryCodes(_state.recoveryCodes);
    const codes = rc[0];
    const hashes = rc[1];

    const now = time.now();
    // Dual-row enrollment (round-7 item 5): write the new secret +
    // recovery codes to the PENDING slot, leaving any existing
    // confirmed enrollment untouched. confirm() promotes pending
    // → main on successful code verify.
    db.batch(() => {
        db.exec("DELETE FROM _hull_totp_pending WHERE user_id = ?",
                [userId]);
        db.exec("DELETE FROM _hull_totp_pending_recovery WHERE user_id = ?",
                [userId]);
        db.exec(
            "INSERT INTO _hull_totp_pending "
            + "(user_id, secret, encrypted, digits, period, created_at) "
            + "VALUES (?, ?, ?, ?, ?, ?)",
            [userId, stored, encryptedFlag,
             _state.digits, _state.period, now]);
        for (let i = 0; i < hashes.length; i++) {
            db.exec(
                "INSERT INTO _hull_totp_pending_recovery "
                + "(user_id, code_hash) VALUES (?, ?)",
                [userId, hashes[i]]);
        }
    });

    const otpauthUrl = buildOtpauthUrl(userId, secretB32);
    const qrSvg = qrcode.svg(otpauthUrl, { ecLevel: "M", scale: 6 });

    return {
        secretBase32:  secretB32,
        otpauthUrl:    otpauthUrl,
        qrSvg:         qrSvg,
        recoveryCodes: codes,
    };
}

function confirm(userId, code) {
    checkInitialized();
    if (typeof userId !== "string" || typeof code !== "string") return false;

    // Confirm verifies against the PENDING enrollment. On success
    // it PROMOTES the pending row into _hull_totp, replacing any
    // existing confirmed row + recovery codes.
    //
    // Round-8 LOW-11: pre-round-8 confirm() fell back to "is the
    // user already enrolled?" when no pending row existed and
    // returned true without verifying any code. Apps that treated
    // confirm()'s boolean as proof-of-possession silently skipped
    // the code check. Call totp.enrolled(userId) for the predicate;
    // confirm() now strictly means "this code verifies pending".
    const pending = loadPendingSecret(userId);
    if (!pending) {
        return false;
    }
    // Round-8 LOW-13: reject pending rows older than pendingTtl
    // even before the daily prune catches them.
    if (pending.created_at != null && _state.pendingTtl
        && (pending.created_at + _state.pendingTtl) < time.now()) {
        return false;
    }

    const nowStep = currentStep();
    for (let offset = -_state.window; offset <= _state.window; offset++) {
        const step = nowStep + offset;
        if (ctEq(totpAtStep(pending.secret, step, pending.digits), code)) {
            const now = time.now();
            db.batch(() => {
                db.exec("DELETE FROM _hull_totp WHERE user_id = ?",
                        [userId]);
                db.exec("DELETE FROM _hull_totp_recovery WHERE user_id = ?",
                        [userId]);
                db.exec(
                    "INSERT INTO _hull_totp "
                    + "(user_id, secret, encrypted, confirmed, digits, "
                    + " period, last_used_step, created_at, updated_at) "
                    + "VALUES (?, ?, ?, 1, ?, ?, ?, ?, ?)",
                    [userId, pending.stored, pending.encrypted,
                     pending.digits, pending.period, step, now, now]);
                db.exec(
                    "INSERT INTO _hull_totp_recovery (user_id, code_hash) "
                    + "SELECT user_id, code_hash FROM "
                    + "_hull_totp_pending_recovery WHERE user_id = ?",
                    [userId]);
                db.exec("DELETE FROM _hull_totp_pending WHERE user_id = ?",
                        [userId]);
                db.exec(
                    "DELETE FROM _hull_totp_pending_recovery "
                    + "WHERE user_id = ?", [userId]);
            });
            return true;
        }
    }
    return false;
}

// Verify a TOTP code or recovery code. Returns `true` on a
// successful verify, `false` otherwise. Use `verifyWithKind` if
// you need to know which of the two paths matched.
//
// History: this used to return a tuple-as-array `[ok, kind]` which
// is truthy regardless of `ok` - a foot-gun for callers writing
// `if (!totp.verify(...)) deny()`. The bare-boolean form is safe
// by default; the tuple is available behind `verifyWithKind`.
function verify(userId, code, req) {
    const r = verifyWithKind(userId, code, req);
    return r[0];
}

function verifyWithKind(userId, code, req) {
    checkInitialized();
    if (typeof userId !== "string" || typeof code !== "string") {
        return [false, null];
    }
    // Round-9 HIGH-4: per-IP gate runs BEFORE the per-user gate so a
    // noisy IP is cut off before it can lock arbitrary victims. req
    // is optional - apps that don't pass it keep round-8 behaviour.
    const ip = req ? extractIp(req) : null;
    // Round-10 HIGH-5: reverted the round-9 dummy PBKDF2. See
    // Lua sibling for the CPU-amplifier reasoning.
    if (ip && lockoutRemainingIp(ip) > 0) {
        return [false, null];
    }
    if (lockoutRemaining(userId) > 0) {
        return [false, null];
    }
    const row = loadSecret(userId);
    if (!row || row.confirmed !== 1) return [false, null];

    const nowStep = currentStep();
    for (let offset = -_state.window; offset <= _state.window; offset++) {
        const step = nowStep + offset;
        if (step > row.lastUsedStep
            && ctEq(totpAtStep(row.secret, step, row.digits), code)) {
            if (markStepUsed(userId, step) === 1) {
                clearFailedAttempts(userId);
                if (ip) clearFailedAttemptsIp(ip);
                // Lazy rekey: re-encrypt under current if the row was
                // stored under an older version.
                if (_state.currentKeyVersion != null
                    && row.version !== _state.currentKeyVersion) {
                    rekeyRow(userId, row.secret);
                }
                return [true, "totp"];
            }
            return [false, null];
        }
    }

    const rows = db.query(
        "SELECT code_hash FROM _hull_totp_recovery "
        + "WHERE user_id = ? AND used_at IS NULL", [userId]);
    for (let i = 0; i < (rows || []).length; i++) {
        if (verifyRecoveryCode(code, rows[i].code_hash)) {
            // Consume atomically: the `used_at IS NULL` filter + affected-row
            // count is the single-use gate. Without it, two concurrent requests
            // presenting the same code both pass the SELECT above and both
            // succeed (double-spend). Losing the race -> deny.
            const consumed = db.exec(
                "UPDATE _hull_totp_recovery SET used_at = ? "
                + "WHERE user_id = ? AND code_hash = ? AND used_at IS NULL",
                [time.now(), userId, rows[i].code_hash]);
            if (consumed !== 1) return [false, null];
            clearFailedAttempts(userId);
            if (ip) clearFailedAttemptsIp(ip);
            // Same lazy rekey on the recovery-code path.
            if (_state.currentKeyVersion != null
                && row.version !== _state.currentKeyVersion) {
                rekeyRow(userId, row.secret);
            }
            return [true, "recovery"];
        }
    }
    bumpFailedAttempt(userId);
    if (ip) bumpFailedAttemptIp(ip);
    return [false, null];
}

/**
 * Batch re-encrypt every stored TOTP secret under the current key
 * version. Use after a key rotation to actively migrate v1 / older
 * v2 rows forward, instead of waiting for every user to sign in
 * (lazy rekey-on-verify happens automatically). Scans the full
 * _hull_totp table once. Returns { scanned, rekeyed, failed }:
 *   - scanned: total rows considered.
 *   - rekeyed: rows successfully re-encrypted under current.
 *   - failed:  rows that wouldn't decrypt under any known key.
 * Operators can decommission an old key from encryptionKeys once
 * rekey() reports failed = 0 AND a follow-up scan reports
 * rekeyed = 0 (every row is already on current).
 */
function rekey() {
    checkInitialized();
    if (_state.currentKeyVersion == null) {
        throw new Error("totp.rekey: no encryptionKeys configured; nothing to do");
    }
    const cur = _state.currentKeyVersion;
    const rows = db.query(
        "SELECT user_id, secret, encrypted FROM _hull_totp");
    let scanned = 0, rekeyed = 0, failed = 0;
    for (let i = 0; i < (rows || []).length; i++) {
        scanned++;
        const r = rows[i];
        const dec = decryptSecret(r.secret, r.encrypted);
        if (!dec[0]) { failed++; continue; }
        if (dec[1] !== cur) {
            const enc = encryptSecret(dec[0]);
            try {
                db.exec(
                    "UPDATE _hull_totp SET secret = ?, encrypted = 1, "
                    + "updated_at = ? WHERE user_id = ?",
                    [enc[0], time.now(), r.user_id]);
                rekeyed++;
            } catch (_e) { failed++; }
        }
    }
    return { scanned: scanned, rekeyed: rekeyed, failed: failed };
}

/**
 * Read-only count of _hull_totp rows grouped by stored key version.
 * Lets operators plan a rotation without writing - call BEFORE
 * rekey() to see how many users are on each version. Returns
 * { [versionId]: count, ..., total: N }. version=0 buckets
 * plaintext rows AND undecryptable rows (legacy v1 without
 * legacyKeyVersion configured, or wrong key).
 */
function rekeyStatus() {
    checkInitialized();
    const rows = db.query("SELECT secret, encrypted FROM _hull_totp");
    const counts = { total: 0 };
    for (let i = 0; i < (rows || []).length; i++) {
        counts.total++;
        const r = rows[i];
        const dec = decryptSecret(r.secret, r.encrypted);
        const k = (dec && dec[1] != null) ? dec[1] : 0;
        counts[k] = (counts[k] || 0) + 1;
    }
    return counts;
}

function disable(userId) {
    checkInitialized();
    if (typeof userId !== "string") return false;
    let removedMain = 0, removedPending = 0;
    db.batch(() => {
        removedMain = db.exec("DELETE FROM _hull_totp WHERE user_id = ?",
                               [userId]);
        db.exec("DELETE FROM _hull_totp_recovery WHERE user_id = ?", [userId]);
        // Round-7 item 5: also wipe any in-flight pending enrollment.
        // "Disabled" means "no enrollment activity for this user" in
        // either slot; report true if EITHER slot had a row.
        removedPending = db.exec(
            "DELETE FROM _hull_totp_pending WHERE user_id = ?", [userId]);
        db.exec("DELETE FROM _hull_totp_pending_recovery WHERE user_id = ?",
                [userId]);
    });
    return (removedMain || 0) + (removedPending || 0) > 0;
}

function enrolled(userId) {
    checkInitialized();
    if (typeof userId !== "string") return false;
    const rows = db.query(
        "SELECT confirmed FROM _hull_totp WHERE user_id = ?", [userId]);
    return rows && rows.length > 0 && rows[0].confirmed === 1;
}

// (totp.middleware was a parallel pending-2FA gate that required apps
// to create a session BEFORE the 2FA step, then flip a flag on it.
// It overlapped with hull/web/auth-flows' totp_pending envelope, which
// defers session creation until after 2FA succeeds. The two could not
// interoperate. Standardized on the auth-flows envelope; apps wire
// TOTP through authFlows.init's `enableTotp / userTotpEnrolled /
// totpVerify` callbacks and let it own the /2fa form + verify route.)

const _test = {
    base32Encode,
    base32Decode,
    totpAtStep,
    currentStep,
    ctEq,
    normalizeRecoveryCode,
    generateRecoveryCodes,
    verifyRecoveryCode,
    encryptSecret,
    decryptSecret,
    buildOtpauthUrl,
    lockoutRemaining,
    bumpFailedAttempt,
    clearFailedAttempts,
    lockoutRemainingIp,
    bumpFailedAttemptIp,
    clearFailedAttemptsIp,
    extractIp,
    xffWarnReset: () => { _xffWarnDone = false; },
    // Test-only: backdate a pending row's created_at so cleanup()
    // finds it stale. See Lua sibling - must run inside the module
    // so the stdlib-caller check admits the _hull_* write.
    forcePendingStale: (userId) => {
        db.exec("UPDATE _hull_totp_pending SET created_at = ? "
              + "WHERE user_id = ?", [0, userId]);
    },
    reset: () => {
        _state.issuer             = "Hull";
        _state.digits             = 6;
        _state.period             = 30;
        _state.window             = 1;
        _state.recoveryCodes      = 10;
        _state.maxFailedAttempts  = 5;
        _state.lockoutDuration    = 15 * 60;
        _state.maxFailedAttemptsPerIp = 20;
        _state.lockoutDurationPerIp   = 15 * 60;
        _state.trustXff               = false;
        _state.pendingTtl         = 3600;
        _state.keys               = {};
        _state.currentKeyVersion  = null;
        _state.legacyKeyVersion   = null;
        _state.cleanupCatchupDone = false;
        _state.cleanupScheduled   = false;
        _state.initialized        = false;
    },
};

function lockoutRemainingPublic(userId) {
    checkInitialized();
    if (typeof userId !== "string") return 0;
    return lockoutRemaining(userId);
}

const totp = { init, enroll, confirm, verify, verifyWithKind,
               rekey, rekeyStatus, disable, enrolled, cleanup,
               lockoutRemaining: lockoutRemainingPublic, _test };
export { totp };
