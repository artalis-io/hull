/**
 * @file hull:jwt
 * @module hull:jwt
 * @description JWT sign / verify / decode.
 *
 * Sign: HS256 only (Hull doesn't ship asymmetric signing keys).
 * Verify: HS256 + RS256/384/512 + PS256 + ES256/384, dispatched by
 * the token's `alg` header against a caller-supplied allowlist.
 *
 * All comparisons of HMAC digests are constant-time. Asym verify
 * delegates to crypto.verify which is constant-time inside the
 * cap layer.
 *
 * Asym-confusion protection: callers MUST pass `opts.algs` (or
 * accept the default `["HS256"]`) to gate which alg values are
 * acceptable. A token claiming RS256 against an HS256-only allowlist
 * is rejected before the key resolver runs. `"none"` is rejected
 * unconditionally.
 *
 * @example HS256 (today's API, unchanged):
 *   const token = jwt.sign({ sub: userId, exp: 3600 }, secret);
 *   const [payload, err] = jwt.verify(token, secret);
 *
 * @example RS256 with a static PEM:
 *   const [payload, err] = jwt.verify(token, pem, { algs: ["RS256"] });
 *
 * @example Asym with JWKS-style key resolver:
 *   function resolver(kid, alg) {
 *       return jwksCache.lookupPem(kid);  // null if unknown
 *   }
 *   const [payload, err] = jwt.verify(token, resolver,
 *                                     { algs: ["RS256", "ES256"] });
 *
 * @license AGPL-3.0-or-later
 */

import { crypto } from "hull:crypto";
import { time } from "hull:time";
import { json } from "hull:json";

// Pre-computed base64url of {"alg":"HS256","typ":"JWT"}, used by sign.
const HEADER_B64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

// The JWS encodings (RFC 7515): base64url without padding, and JSON text as
// UTF-8. A segment that is not valid base64url, or does not decode to UTF-8,
// is refused - the Lua module applies exactly the same rules.
function segmentText(s) {
    const raw = encoding.base64.decode(s, { url: true });
    return raw === null ? null : encoding.utf8.decode(raw);
}

const EXP_RELATIVE_THRESHOLD = 2e9;

// Allowlist of algs this module understands. Token-supplied algs
// outside this set are rejected before the resolver runs.
const SUPPORTED_ALGS = new Set([
    "HS256",
    "RS256", "RS384", "RS512",
    "PS256",
    "ES256", "ES384",
]);

// Constant-time comparison, done in C (crypto.constantTimeEq) so the timing is
// not subject to interpreter variance. Length leak is acceptable: both inputs
// are fixed-length HMAC outputs.
function constantTimeCompare(a, b) {
    return crypto.constantTimeEq(a, b);
}

import { encoding } from "hull:encoding";

// HS256: HMAC-SHA256 over the signing input, returning the
// base64url-encoded 32-byte digest (matches what the JWS token holds).
function hs256SignatureB64(signingInput, secret) {
    const keyHex = encoding.hex.encode(secret);
    const sigHex = crypto.hmacSha256(signingInput, keyHex);
    // Byte-for-byte what the Lua sibling produces (tests/e2e_token_interop.sh).
    return encoding.base64.encode(encoding.hex.decode(sigHex), { url: true });
}

/**
 * Sign a payload and return a JWT string (HS256 only).
 *
 * @param {Object} payload  Claims to encode.
 * @param {string} secret   HMAC-SHA256 key (ASCII).
 * @returns {string}        JWT in compact form.
 */
function sign(payload, secret) {
    if (!payload || typeof payload !== "object")
        throw new Error("payload must be an object");
    if (!secret || typeof secret !== "string")
        throw new Error("secret is required");
    // SECURITY: use a 32+ byte random HMAC key. A short (<16 byte) HS256 secret
    // is brute-forceable. Not hard-rejected here (would break existing apps),
    // but strongly recommended; oauth.init enforces >=16 for its state secret.

    const p = Object.create(null);
    for (const k of Object.keys(payload)) p[k] = payload[k];
    if (p.iat === undefined) p.iat = time.now();
    if (typeof p.exp === "number" && p.exp < EXP_RELATIVE_THRESHOLD)
        p.exp = time.now() + p.exp;

    const payloadB64 = encoding.base64.encode(encoding.utf8.encode(json.encode(p)), { url: true });
    const signingInput = HEADER_B64 + "." + payloadB64;
    const sigB64 = hs256SignatureB64(signingInput, secret);
    return signingInput + "." + sigB64;
}

// Resolve keyOrResolver to the actual key bytes. If callable, invoke
// with (kid, alg) so JWKS callers can route by the token's key-id
// header. If string, return as-is.
function resolveKey(keyOrResolver, kid, alg) {
    if (typeof keyOrResolver === "function") {
        return keyOrResolver(kid, alg);
    }
    return keyOrResolver;
}

// The signature bytes of an asymmetric JWS, or null. Strict base64url, the
// same decoding the Lua sibling applies: no padding, no '+' or '/', so a
// token verifies in both runtimes or in neither.
function signatureBytes(sigB64) {
    const raw = encoding.base64.decode(sigB64, { url: true });
    return raw === null ? null : encoding.bytes.toU8(raw);
}

function verifySignature(alg, key, signingInput, sigB64) {
    if (alg === "HS256") {
        if (typeof key !== "string" || key.length === 0) return false;
        const expected = hs256SignatureB64(signingInput, key);
        return constantTimeCompare(sigB64, expected);
    }
    // Asym: crypto.verify takes the raw r||s sig (for ECDSA), which is
    // exactly what's encoded in the JWS token (per RFC 7515 §3.1).
    if (typeof key !== "string" || key.length === 0) return false;
    const sigBytes = signatureBytes(sigB64);
    if (sigBytes === null) return false;
    return crypto.verify(alg, key, signingInput, sigBytes.buffer);
}

/**
 * Verify a JWT and return the decoded payload.
 *
 * @param {string} token  JWT in compact form.
 * @param {string|Function} keyOrResolver  Either:
 *   - HMAC secret (HS256) or PEM-encoded SubjectPublicKeyInfo (asym);
 *   - a function `(kid, alg) => key` for JWKS-style resolution.
 *     Returning a falsy value from the resolver fails the verify.
 * @param {Object} [opts]
 * @param {string[]} [opts.algs=["HS256"]]  Allowlist of acceptable
 *   alg values. MUST include the asym algs you intend to accept.
 * @param {boolean} [opts.requireExp]   Reject tokens missing `exp`.
 * @param {boolean} [opts.require_exp]  Lua-parity alias.
 * @returns {[Object|null, string|null]}  `[payload, null]` on success,
 *   `[null, reason]` on failure.
 */
function verify(token, keyOrResolver, opts) {
    // A missing KEY is a precondition failure (you cannot verify without one) ->
    // throw, matching the Lua sibling (docs/stdlib_style.md section 1). A
    // missing/malformed TOKEN is untrusted input -> return [null, reason], the
    // expected-negative outcome a verifier reports.
    if (keyOrResolver === undefined || keyOrResolver === null)
        throw new Error("jwt.verify: key is required");
    if (!token || typeof token !== "string")
        return [null, "invalid token"];
    opts = opts || {};
    const allowed = opts.algs || ["HS256"];

    const parts = token.split(".", 4);
    if (parts.length !== 3)
        return [null, "malformed token"];

    // Index access rather than array destructuring: QuickJS's parser
    // trips an MSan use-of-uninit warning on `const [a,b,c] = parts;`
    // when this module is compiled fresh through the bytecode cache.
    // The parser bug is upstream; the workaround is cheap.
    const headerB64  = parts[0];
    const payloadB64 = parts[1];
    const sigB64     = parts[2];

    const headerJson = segmentText(headerB64);
    if (headerJson === null) return [null, "invalid header encoding"];
    let header;
    try { header = json.decode(headerJson); }
    catch (e) { return [null, "invalid header JSON"]; }
    if (!header || typeof header !== "object")
        return [null, "invalid header"];

    const alg = header.alg;
    if (!alg || alg === "none")
        return [null, "alg 'none' rejected"];
    if (!SUPPORTED_ALGS.has(alg))
        return [null, "unsupported algorithm: " + String(alg)];

    // Allowlist enforcement BEFORE the key resolver runs.
    if (allowed.indexOf(alg) === -1)
        return [null, "alg " + alg + " not in allowed list"];

    const key = resolveKey(keyOrResolver, header.kid, alg);
    if (!key) return [null, "no key for kid/alg"];

    const signingInput = headerB64 + "." + payloadB64;
    if (!verifySignature(alg, key, signingInput, sigB64))
        return [null, "invalid signature"];

    const payloadStr = segmentText(payloadB64);
    if (payloadStr === null) return [null, "invalid payload encoding"];
    let payload;
    try { payload = json.decode(payloadStr); }
    catch (e) { return [null, "invalid payload JSON"]; }
    if (!payload || typeof payload !== "object")
        return [null, "payload is not an object"];

    // exp / nbf must be numbers per RFC 7519 §4.1.4 / §4.1.5. A token
    // crafted with `"exp": "not-a-number"` would otherwise compare
    // string vs number (JS implicit-coerces, but the result is NaN
    // and the >= test returns false, silently treating the token as
    // valid forever). Reject malformed claims explicitly.
    if (payload.exp !== undefined) {
        if (typeof payload.exp !== "number")
            return [null, "invalid exp claim"];
        if (time.now() >= payload.exp) return [null, "token expired"];
    } else if (opts.requireExp || opts.require_exp) {
        return [null, "token missing required exp claim"];
    }
    if (payload.nbf !== undefined) {
        if (typeof payload.nbf !== "number")
            return [null, "invalid nbf claim"];
        if (time.now() < payload.nbf) return [null, "token not yet valid"];
    }

    return [payload, null];
}

/**
 * Decode a JWT payload WITHOUT verifying the signature. Debug use only.
 */
function decode(token) {
    if (!token || typeof token !== "string") return null;
    const parts = token.split(".", 4);
    if (parts.length !== 3) return null;
    const payloadStr = segmentText(parts[1]);
    if (payloadStr === null) return null;
    let payload;
    try { payload = json.decode(payloadStr); }
    catch (e) { return null; }
    // An object or nothing, as in Lua (and as verify requires): a payload that
    // is `5` is not a claim set.
    return (payload && typeof payload === "object") ? payload : null;
}

const jwt = { sign, verify, decode };
export { jwt };
