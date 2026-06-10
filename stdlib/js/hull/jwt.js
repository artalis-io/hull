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

const EXP_RELATIVE_THRESHOLD = 2e9;

// Allowlist of algs this module understands. Token-supplied algs
// outside this set are rejected before the resolver runs.
const SUPPORTED_ALGS = new Set([
    "HS256",
    "RS256", "RS384", "RS512",
    "PS256",
    "ES256", "ES384",
]);

function constantTimeCompare(a, b) {
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; i++)
        diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
    return diff === 0;
}

function secretToHex(secret) {
    let hex = "";
    for (let i = 0; i < secret.length; i++) {
        const c = secret.charCodeAt(i);
        if (c > 127) throw new Error("jwt: secret must be ASCII-only");
        hex += c.toString(16).padStart(2, "0");
    }
    return hex;
}

// HS256: HMAC-SHA256 over the signing input, returning the
// base64url-encoded 32-byte digest (matches what the JWS token holds).
function hs256SignatureB64(signingInput, secret) {
    const keyHex = secretToHex(secret);
    const sigHex = crypto.hmacSha256(signingInput, keyHex);
    let raw = "";
    for (let i = 0; i < sigHex.length; i += 2)
        raw += String.fromCharCode(parseInt(sigHex.substring(i, i + 2), 16));
    return crypto.base64urlEncode(raw);
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

    const p = Object.create(null);
    for (const k of Object.keys(payload)) p[k] = payload[k];
    if (p.iat === undefined) p.iat = time.now();
    if (typeof p.exp === "number" && p.exp < EXP_RELATIVE_THRESHOLD)
        p.exp = time.now() + p.exp;

    const payloadB64 = crypto.base64urlEncode(json.encode(p));
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

// QuickJS strings are not byte-arrays. Routing raw binary through
// crypto.base64urlDecode -> JS string -> Uint8Array corrupts any
// byte >= 128 because the string layer treats input as UTF-8 and
// silently substitutes / inflates on invalid sequences. JWT
// signatures (RSA, ECDSA) are uniformly binary, so we need a
// direct base64url -> Uint8Array path that never touches strings.
const B64_DEC = (() => {
    const t = new Int8Array(256).fill(-1);
    const a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (let i = 0; i < a.length; i++) t[a.charCodeAt(i)] = i;
    // Tolerate standard base64 too (+ and /).
    t["+".charCodeAt(0)] = 62;
    t["/".charCodeAt(0)] = 63;
    return t;
})();
// Decode a single base64 char via the lookup table, guarding the
// `s.charCodeAt(i) > 255` case explicitly. B64_DEC is Int8Array(256);
// indexing past 255 returns undefined, and `(undef | x) < 0` is
// false because undef coerces to 0 — so without this guard a token
// containing a non-ASCII char would silently zero-pad and produce
// garbage bytes (rejected at the crypto layer, but masked as a
// generic sig failure).
function b64Lookup(s, i) {
    const c = s.charCodeAt(i);
    if (c > 255) return -1;
    return B64_DEC[c];
}

function base64urlToBytes(s) {
    // Strip padding if any (JWT compact form omits it).
    let n = s.length;
    while (n > 0 && s.charCodeAt(n - 1) === 61 /* "=" */) n--;
    const groups = n >> 2;
    const tail = n & 3;
    if (tail === 1) return null;  // invalid base64 length
    let outLen = groups * 3;
    if (tail === 2) outLen += 1;
    else if (tail === 3) outLen += 2;
    const u8 = new Uint8Array(outLen);
    let si = 0, di = 0;
    for (let g = 0; g < groups; g++) {
        const a = b64Lookup(s, si++);
        const b = b64Lookup(s, si++);
        const c = b64Lookup(s, si++);
        const d = b64Lookup(s, si++);
        if ((a | b | c | d) < 0) return null;
        u8[di++] = (a << 2) | (b >> 4);
        u8[di++] = ((b & 0xf) << 4) | (c >> 2);
        u8[di++] = ((c & 0x3) << 6) | d;
    }
    if (tail === 2) {
        const a = b64Lookup(s, si++);
        const b = b64Lookup(s, si++);
        if ((a | b) < 0) return null;
        u8[di++] = (a << 2) | (b >> 4);
    } else if (tail === 3) {
        const a = b64Lookup(s, si++);
        const b = b64Lookup(s, si++);
        const c = b64Lookup(s, si++);
        if ((a | b | c) < 0) return null;
        u8[di++] = (a << 2) | (b >> 4);
        u8[di++] = ((b & 0xf) << 4) | (c >> 2);
    }
    return u8;
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
    const sigBytes = base64urlToBytes(sigB64);
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
    if (!token || typeof token !== "string")
        return [null, "invalid token"];
    if (!keyOrResolver)
        return [null, "key is required"];
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

    const headerJson = crypto.base64urlDecode(headerB64);
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

    const payloadStr = crypto.base64urlDecode(payloadB64);
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
    const payloadStr = crypto.base64urlDecode(parts[1]);
    if (payloadStr === null) return null;
    try { return json.decode(payloadStr); }
    catch (e) { return null; }
}

const jwt = { sign, verify, decode };
export { jwt };
