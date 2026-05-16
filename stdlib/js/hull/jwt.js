/**
 * @file hull:jwt
 * @module hull:jwt
 * @description JWT HS256 sign / verify / decode.
 *
 * Only HS256 is supported — `"alg":"none"` is rejected, and there is no
 * asymmetric-key path here. All comparisons of HMAC digests are
 * constant-time.
 *
 * @example
 * import { jwt } from "hull:jwt";
 * const token = jwt.sign({ sub: userId, exp: 3600 }, secret);  // 1h from now
 * const [payload, err] = jwt.verify(token, secret, { requireExp: true });
 *
 * @license AGPL-3.0-or-later
 */

import { crypto } from "hull:crypto";
import { time } from "hull:time";
import { json } from "hull:json";

// Pre-computed base64url of {"alg":"HS256","typ":"JWT"}
const HEADER_B64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

// Lua-jwt parity: if payload.exp is below this threshold (~2033), treat
// it as a duration in seconds-from-now rather than an absolute Unix
// timestamp. Lets callers write `{ exp: 3600 }` meaning "1 hour from
// now" without manually adding time.now().
const EXP_RELATIVE_THRESHOLD = 2e9;

// Length leak is acceptable: both inputs are always base64url of
// 32-byte HMAC-SHA256 output (43 chars), so lengths always match
// for well-formed tokens. A truncated signature fails the length
// check but reveals no useful information.
function constantTimeCompare(a, b) {
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; i++)
        diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
    return diff === 0;
}

// Secret must be ASCII-only; charCodeAt returns 16-bit code units
// for non-ASCII which would produce inconsistent HMAC keys.
function secretToHex(secret) {
    let hex = "";
    for (let i = 0; i < secret.length; i++) {
        const c = secret.charCodeAt(i);
        if (c > 127) throw new Error("jwt: secret must be ASCII-only");
        hex += c.toString(16).padStart(2, "0");
    }
    return hex;
}

function computeSignature(headerB64, payloadB64, secret) {
    const signingInput = headerB64 + "." + payloadB64;
    const keyHex = secretToHex(secret);
    const sigHex = crypto.hmacSha256(signingInput, keyHex);

    // Convert hex to raw bytes string for base64url encoding
    let raw = "";
    for (let i = 0; i < sigHex.length; i += 2)
        raw += String.fromCharCode(parseInt(sigHex.substring(i, i + 2), 16));

    return crypto.base64urlEncode(raw);
}

/**
 * Sign a payload and return a JWT string.
 *
 * Special claim handling:
 *   - `iat` is auto-set to `time.now()` if absent.
 *   - `exp` < 2e9 is treated as seconds-from-now and added to `time.now()`.
 *     Values ≥ 2e9 are taken as absolute Unix timestamps. (Lua parity.)
 *
 * @param {Object} payload  Claims to encode. Any JSON-serializable values.
 * @param {string} secret   HMAC-SHA256 key. ASCII bytes; must not be empty.
 * @returns {string}        Compact-encoded JWT (`base64url(header).base64url(payload).base64url(sig)`).
 * @throws {Error} If `payload` is not an object or `secret` is missing.
 *
 * @example
 * const token = sign({ sub: "alice", exp: 3600 }, "my-secret");
 */
function sign(payload, secret) {
    if (!payload || typeof payload !== "object")
        throw new Error("payload must be an object");
    if (!secret || typeof secret !== "string")
        throw new Error("secret is required");

    // Copy payload via own-property iteration so we don't accidentally
    // promote an inherited or __proto__-set key into the JWT.
    const p = Object.create(null);
    for (const k of Object.keys(payload)) p[k] = payload[k];
    if (p.iat === undefined)
        p.iat = time.now();

    // Lua-jwt parity (M-1): if exp is small enough to look like a
    // duration (e.g. 3600) rather than an absolute timestamp, add now.
    // Phase 6 audit M-2: drop the `> 0` guard so a caller passing
    // `exp: 0` or a negative value gets the same "subtract from now"
    // treatment as Lua (both runtimes produce expired tokens, but the
    // branch shape is now identical).
    if (typeof p.exp === "number" && p.exp < EXP_RELATIVE_THRESHOLD)
        p.exp = time.now() + p.exp;

    const payloadB64 = crypto.base64urlEncode(json.encode(p));
    const sig = computeSignature(HEADER_B64, payloadB64, secret);

    return HEADER_B64 + "." + payloadB64 + "." + sig;
}

/**
 * Verify a JWT and return the decoded payload.
 *
 * Steps performed:
 *   1. Split the token into header / payload / signature.
 *   2. Check the header matches the canonical HS256 header.
 *   3. Recompute HMAC-SHA256, constant-time-compare.
 *   4. Decode payload JSON.
 *   5. Reject expired (`exp` ≤ now) or not-yet-valid (`nbf` > now).
 *   6. If `opts.requireExp` / `opts.require_exp`, reject tokens with no `exp`.
 *
 * @param {string} token       JWT in compact form.
 * @param {string} secret      Same secret used at sign time.
 * @param {Object} [opts]      Verification options.
 * @param {boolean} [opts.requireExp]   Reject tokens missing `exp`.
 * @param {boolean} [opts.require_exp]  Lua-parity alias.
 * @returns {[Object|null, string|null]}
 *   On success: `[payload, null]`. On failure: `[null, reason]` where reason is one of
 *   `"invalid token"`, `"secret is required"`, `"malformed token"`,
 *   `"unsupported algorithm"`, `"invalid signature"`, `"invalid payload encoding"`,
 *   `"invalid payload JSON"`, `"payload is not an object"`, `"token expired"`,
 *   `"token missing required exp claim"`, `"token not yet valid"`.
 *
 * @example
 * const [payload, err] = verify(token, secret, { requireExp: true });
 * if (!payload) return res.status(401).json({ error: err });
 */
function verify(token, secret, opts) {
    if (!token || typeof token !== "string")
        return [null, "invalid token"];
    if (!secret || typeof secret !== "string")
        return [null, "secret is required"];
    opts = opts || {};

    const parts = token.split(".", 4);
    if (parts.length !== 3)
        return [null, "malformed token"];

    // Verify header
    if (parts[0] !== HEADER_B64)
        return [null, "unsupported algorithm"];

    // Verify signature
    const expectedSig = computeSignature(parts[0], parts[1], secret);
    if (!constantTimeCompare(parts[2], expectedSig))
        return [null, "invalid signature"];

    // Decode payload
    const payloadStr = crypto.base64urlDecode(parts[1]);
    if (payloadStr === null)
        return [null, "invalid payload encoding"];

    let payload;
    try {
        payload = json.decode(payloadStr);
    } catch (e) {
        return [null, "invalid payload JSON"];
    }

    if (!payload || typeof payload !== "object")
        return [null, "payload is not an object"];

    // Check expiration. Lua-jwt parity (M-1): opts.requireExp /
    // opts.require_exp rejects tokens lacking an exp claim.
    if (payload.exp !== undefined) {
        const now = time.now();
        if (now >= payload.exp)
            return [null, "token expired"];
    } else if (opts.requireExp || opts.require_exp) {
        return [null, "token missing required exp claim"];
    }

    // Check not-before
    if (payload.nbf !== undefined) {
        const now = time.now();
        if (now < payload.nbf)
            return [null, "token not yet valid"];
    }

    return [payload, null];
}

/**
 * Decode a JWT payload WITHOUT verifying the signature. Debug use only.
 *
 * @param {string} token  JWT in compact form.
 * @returns {Object|null} Decoded payload, or `null` on malformed input.
 *
 * @warning Never use this for authentication. Always pair with {@link verify}.
 */
function decode(token) {
    if (!token || typeof token !== "string")
        return null;

    const parts = token.split(".", 4);
    if (parts.length !== 3)
        return null;

    const payloadStr = crypto.base64urlDecode(parts[1]);
    if (payloadStr === null)
        return null;

    try {
        return json.decode(payloadStr);
    } catch (e) {
        return null;
    }
}

const jwt = { sign, verify, decode };
export { jwt };
