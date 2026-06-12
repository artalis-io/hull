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
 *   totp.verify(userId, code)  -> [ok, "totp" | "recovery" | null]
 *   totp.disable(userId)
 *   totp.enrolled(userId)      -> bool
 *   totp.middleware({ redirectPath, sessionKey, skipPaths })
 */

import { crypto } from "hull:crypto";
import { db }     from "hull:db";
import { time }   from "hull:time";
import { qrcode } from "hull:qrcode";

// ── Module state ───────────────────────────────────────────────────

const _state = {
    issuer:           "Hull",
    digits:           6,
    period:           30,
    window:           1,
    recoveryCodes:    10,
    encryptionKey:    null,
    encryptionKeyHex: null,
    initialized:      false,
};

// ── Schema ─────────────────────────────────────────────────────────

const SCHEMA = `
CREATE TABLE IF NOT EXISTS _hull_totp (
    user_id        TEXT PRIMARY KEY,
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
    user_id   TEXT NOT NULL,
    code_hash TEXT NOT NULL,
    used_at   INTEGER,
    PRIMARY KEY (user_id, code_hash)
);

CREATE INDEX IF NOT EXISTS _hull_totp_recovery_user
    ON _hull_totp_recovery(user_id);
`;

// ── Helpers ────────────────────────────────────────────────────────

function bytesToHex(s) {
    let h = "";
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i) & 0xff;
        h += (c < 16 ? "0" : "") + c.toString(16);
    }
    return h;
}

function hexToBytes(h) {
    let s = "";
    for (let i = 0; i < h.length; i += 2) {
        s += String.fromCharCode(parseInt(h.substr(i, 2), 16));
    }
    return s;
}

// RFC 4648 Base32 (no padding) — encode + decode. 20 bytes → 32
// chars; decoder is case-insensitive and tolerates "=" / whitespace.
const B32_ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
const B32_INV = (() => {
    const t = {};
    for (let i = 0; i < B32_ALPHA.length; i++) t[B32_ALPHA.charCodeAt(i)] = i;
    return t;
})();

function base32Encode(bytes) {
    let out = "";
    let buf = 0, bits = 0;
    for (let i = 0; i < bytes.length; i++) {
        buf = (buf << 8) | (bytes.charCodeAt(i) & 0xff);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            const v = (buf >> bits) & 0x1F;
            out += B32_ALPHA[v];
            buf &= (1 << bits) - 1;
        }
    }
    if (bits > 0) {
        const v = (buf << (5 - bits)) & 0x1F;
        out += B32_ALPHA[v];
    }
    return out;
}

function base32Decode(s) {
    if (typeof s !== "string") return null;
    let out = "";
    let buf = 0, bits = 0;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c === 32 || c === 9 || c === 10 || c === 13 || c === 0x3D) continue;
        const up = c >= 97 && c <= 122 ? c - 32 : c;  // ASCII uppercase
        const v = B32_INV[up];
        if (v === undefined) return null;
        buf = (buf << 5) | v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out += String.fromCharCode((buf >> bits) & 0xff);
            buf &= (1 << bits) - 1;
        }
    }
    return out;
}

// TOTP digest per RFC 4226 §5.3-5.4. Computes the 8-byte big-endian
// counter as two 32-bit halves so we avoid JS's 53-bit Number cap
// (step counters fit comfortably in 53 bits but BITWISE OPS truncate
// to 32 bits, so we split manually). The dynamic-truncation step uses
// multiplication to keep the 31-bit accumulator inside Number's
// integer-safe range without any signed-shift surprises.
function totpAtStep(secretBytes, step, digits) {
    const hi = Math.floor(step / 0x100000000);
    const lo = step - hi * 0x100000000;
    /* Use a Uint8Array (via .buffer) so crypto.hmacSha1 takes the
     * raw bytes through js_get_buffer's TypedArray probe. A plain
     * JS string of high-byte chars would UTF-8-inflate at the C
     * boundary and produce the wrong MAC — silently — for any
     * counter byte >= 0x80. */
    const counter = new Uint8Array([
        (hi >>> 24) & 0xff, (hi >>> 16) & 0xff,
        (hi >>>  8) & 0xff,  hi         & 0xff,
        (lo >>> 24) & 0xff, (lo >>> 16) & 0xff,
        (lo >>>  8) & 0xff,  lo         & 0xff,
    ]).buffer;
    const keyHex = bytesToHex(secretBytes);
    const macHex = crypto.hmacSha1(counter, keyHex);
    const mac = new Array(20);
    for (let i = 0; i < 20; i++) mac[i] = parseInt(macHex.substr(i * 2, 2), 16);
    const offset = mac[19] & 0x0F;
    const p = (mac[offset] & 0x7F) * 0x1000000
            + (mac[offset + 1] & 0xFF) * 0x10000
            + (mac[offset + 2] & 0xFF) * 0x100
            + (mac[offset + 3] & 0xFF);
    const mod = Math.pow(10, digits);
    const code = p % mod;
    return String(code).padStart(digits, "0");
}

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
// in depth. Charcode XOR-fold mirrors the Lua side's ct_eq.
function ctEq(a, b) {
    if (typeof a !== "string" || typeof b !== "string") return false;
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; i++) {
        diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
    }
    return diff === 0;
}

// At-rest encryption: nonce(24) || ct.  Same wire format as the Lua
// module so a Lua-Hull migration to JS-Hull (or vice versa) on the
// same DB works without re-enrolling users.
const NONCE_LEN = 24;
const MAC_LEN   = 16;

function encryptSecret(secretBytes) {
    if (!_state.encryptionKeyHex) return [secretBytes, 0];
    const nonce = new Uint8Array(crypto.random(NONCE_LEN));
    let nonceStr = "";
    for (let i = 0; i < NONCE_LEN; i++) nonceStr += String.fromCharCode(nonce[i]);
    const nonceHex = bytesToHex(nonceStr);
    const ctHex = crypto.secretbox(secretBytes, nonceHex,
                                    _state.encryptionKeyHex);
    return [nonceStr + hexToBytes(ctHex), 1];
}

function decryptSecret(blob, encrypted) {
    if (encrypted === 0) return blob;
    if (!_state.encryptionKeyHex) return null;
    if (blob.length < NONCE_LEN + MAC_LEN) return null;
    const nonceStr = blob.substring(0, NONCE_LEN);
    const ctStr    = blob.substring(NONCE_LEN);
    const pt = crypto.secretboxOpen(bytesToHex(ctStr),
                                     bytesToHex(nonceStr),
                                     _state.encryptionKeyHex);
    return pt;  // null on tamper / wrong key
}

function urlenc(s) {
    return encodeURIComponent(s).replace(/[!'()*]/g, (c) =>
        "%" + c.charCodeAt(0).toString(16).toUpperCase());
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
    const secret = decryptSecret(row.secret, row.encrypted);
    if (!secret) return null;
    return {
        secret:        secret,
        confirmed:     row.confirmed,
        digits:        row.digits,
        period:        row.period,
        lastUsedStep:  row.last_used_step,
    };
}

function markStepUsed(userId, step) {
    return db.exec(
        "UPDATE _hull_totp SET last_used_step = ?, updated_at = ? "
        + "WHERE user_id = ? AND last_used_step < ?",
        [step, time.now(), userId, step]);
}

function currentStep() {
    return Math.floor(time.now() / _state.period);
}

function checkInitialized() {
    if (!_state.initialized) {
        throw new Error("totp: call totp.init(...) before any other function");
    }
}

// ── Public API ─────────────────────────────────────────────────────

function init(opts) {
    opts = opts || {};
    if (opts.digits !== undefined && opts.digits !== 6 && opts.digits !== 8) {
        throw new Error("totp.init: digits must be 6 or 8");
    }
    if (opts.encryptionKey !== undefined) {
        if (typeof opts.encryptionKey !== "string") {
            throw new Error("totp.init: encryptionKey must be a string");
        }
        if (opts.encryptionKey.length !== 32) {
            throw new Error("totp.init: encryptionKey must be exactly 32 bytes");
        }
    }
    _state.issuer         = opts.issuer         || _state.issuer;
    _state.digits         = opts.digits         || _state.digits;
    _state.period         = opts.period         || _state.period;
    _state.window         = opts.window         !== undefined ? opts.window : _state.window;
    _state.recoveryCodes  = opts.recoveryCodes  || _state.recoveryCodes;
    _state.encryptionKey  = opts.encryptionKey  || null;
    _state.encryptionKeyHex = opts.encryptionKey
        ? bytesToHex(opts.encryptionKey)
        : null;

    db.batch(() => {
        const stmts = SCHEMA.split(";");
        for (let i = 0; i < stmts.length; i++) {
            const s = stmts[i].trim();
            if (s.length > 0) db.exec(s);
        }
    });

    _state.initialized = true;
}

function enroll(userId) {
    checkInitialized();
    if (typeof userId !== "string" || userId === "") {
        throw new Error("totp.enroll: userId required");
    }

    const randAb = crypto.random(20);
    const u8 = new Uint8Array(randAb);
    let secretBytes = "";
    for (let i = 0; i < 20; i++) secretBytes += String.fromCharCode(u8[i]);

    const secretB32 = base32Encode(secretBytes);
    const enc = encryptSecret(secretBytes);
    const stored = enc[0];
    const encryptedFlag = enc[1];

    const rc = generateRecoveryCodes(_state.recoveryCodes);
    const codes = rc[0];
    const hashes = rc[1];

    const now = time.now();
    db.batch(() => {
        db.exec("DELETE FROM _hull_totp WHERE user_id = ?", [userId]);
        db.exec("DELETE FROM _hull_totp_recovery WHERE user_id = ?", [userId]);
        db.exec(
            "INSERT INTO _hull_totp "
            + "(user_id, secret, encrypted, confirmed, digits, period, "
            + " last_used_step, created_at, updated_at) "
            + "VALUES (?, ?, ?, 0, ?, ?, -1, ?, ?)",
            [userId, stored, encryptedFlag,
             _state.digits, _state.period, now, now]);
        for (let i = 0; i < hashes.length; i++) {
            db.exec(
                "INSERT INTO _hull_totp_recovery (user_id, code_hash) "
                + "VALUES (?, ?)", [userId, hashes[i]]);
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
    const row = loadSecret(userId);
    if (!row) return false;
    if (row.confirmed === 1) return true;

    const nowStep = currentStep();
    for (let offset = -_state.window; offset <= _state.window; offset++) {
        const step = nowStep + offset;
        if (ctEq(totpAtStep(row.secret, step, row.digits), code)) {
            db.batch(() => {
                db.exec(
                    "UPDATE _hull_totp SET confirmed = 1, "
                    + "last_used_step = ?, updated_at = ? "
                    + "WHERE user_id = ?",
                    [step, time.now(), userId]);
            });
            return true;
        }
    }
    return false;
}

function verify(userId, code) {
    checkInitialized();
    if (typeof userId !== "string" || typeof code !== "string") {
        return [false, null];
    }
    const row = loadSecret(userId);
    if (!row || row.confirmed !== 1) return [false, null];

    const nowStep = currentStep();
    for (let offset = -_state.window; offset <= _state.window; offset++) {
        const step = nowStep + offset;
        if (step > row.lastUsedStep
            && ctEq(totpAtStep(row.secret, step, row.digits), code)) {
            if (markStepUsed(userId, step) === 1) return [true, "totp"];
            return [false, null];
        }
    }

    const rows = db.query(
        "SELECT code_hash FROM _hull_totp_recovery "
        + "WHERE user_id = ? AND used_at IS NULL", [userId]);
    for (let i = 0; i < (rows || []).length; i++) {
        if (verifyRecoveryCode(code, rows[i].code_hash)) {
            db.exec(
                "UPDATE _hull_totp_recovery SET used_at = ? "
                + "WHERE user_id = ? AND code_hash = ?",
                [time.now(), userId, rows[i].code_hash]);
            return [true, "recovery"];
        }
    }
    return [false, null];
}

function disable(userId) {
    checkInitialized();
    if (typeof userId !== "string") return false;
    let removed = 0;
    db.batch(() => {
        removed = db.exec("DELETE FROM _hull_totp WHERE user_id = ?", [userId]);
        db.exec("DELETE FROM _hull_totp_recovery WHERE user_id = ?", [userId]);
    });
    return removed > 0;
}

function enrolled(userId) {
    checkInitialized();
    if (typeof userId !== "string") return false;
    const rows = db.query(
        "SELECT confirmed FROM _hull_totp WHERE user_id = ?", [userId]);
    return rows && rows.length > 0 && rows[0].confirmed === 1;
}

function middleware(opts) {
    checkInitialized();
    opts = opts || {};
    const redirectPath = opts.redirectPath || "/2fa";
    const sessionKey   = opts.sessionKey   || "pending_2fa";
    const skipPaths    = opts.skipPaths    || ["/2fa", "/logout"];
    const skip = {};
    for (let i = 0; i < skipPaths.length; i++) skip[skipPaths[i]] = true;

    return (req, res) => {
        if (skip[req.path]) return 0;
        const sess = req.ctx && req.ctx.session;
        if (!sess || !sess[sessionKey]) return 0;
        res.redirect(redirectPath);
        return 1;
    };
}

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
    reset: () => {
        _state.issuer            = "Hull";
        _state.digits            = 6;
        _state.period            = 30;
        _state.window            = 1;
        _state.recoveryCodes     = 10;
        _state.encryptionKey     = null;
        _state.encryptionKeyHex  = null;
        _state.initialized       = false;
    },
};

const totp = { init, enroll, confirm, verify, disable, enrolled, middleware, _test };
export { totp };
