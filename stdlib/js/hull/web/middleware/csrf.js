/**
 * @file hull:web:middleware:csrf
 * @module hull:web:middleware:csrf
 * @description Stateless CSRF tokens via HMAC-SHA256. Lua parity:
 *   `hull.web.middleware.csrf`.
 *
 * Wire format (shared with Lua sibling): `tsHex.hmac_hex`.
 *   tsHex   = unix timestamp formatted as compact lowercase hex
 *             (e.g. `"664f4f80"`).
 *   hmac_hex = HMAC-SHA256(secret, `session_id ":" tsHex`).
 *
 * The `:` separator inside the HMAC input is deliberately distinct
 * from the `.` separator in the wire token, so a session id that
 * contains `.` cannot be confused with the tsHex prefix. Tokens minted
 * by one runtime verify in the other; cross-runtime fixture is in
 * `tests/e2e_csrf_parity.sh`.
 *
 * Only relevant for cookie/session-based auth. JWT Bearer auth does not
 * need CSRF protection.
 *
 * @license AGPL-3.0-or-later
 */

import { crypto } from "hull:crypto";
import { cookie } from "hull:web:cookie";
import { time } from "hull:time";

import { _hex } from "hull:crypto:_hex";
// Raw-byte hex (byte-consistent with Lua; NOT crypto.hexEncode which
// UTF-8-inflates in JS). Accepts any byte, so non-ASCII secrets work too.
const secretToHex = _hex.toHex;

function computeHmac(sessionId, tsHex, secret) {
    // ":" separator deliberately distinct from the wire-format "."
    // separator — see the wire-format note at the top of this file.
    const msg = sessionId + ":" + tsHex;
    const keyHex = secretToHex(secret);
    return crypto.hmacSha256(msg, keyHex);
}

/**
 * Generate a CSRF token for `(sessionId, secret)`.
 *
 * @param {string} sessionId
 * @param {string} secret  HMAC key.
 * @returns {string}  `hex_timestamp.hmac_hex`.
 */
function generate(sessionId, secret) {
    if (!sessionId || typeof sessionId !== "string")
        throw new Error("sessionId is required");
    if (!secret || typeof secret !== "string")
        throw new Error("secret is required");

    const now = time.now();
    const tsHex = now.toString(16);
    const mac = computeHmac(sessionId, tsHex, secret);

    return tsHex + "." + mac;
}

/**
 * Verify a CSRF token (constant-time HMAC compare + age check).
 *
 * @param {string} token
 * @param {string} sessionId
 * @param {string} secret
 * @param {number} [ttl=3600]  Max token age in seconds.
 * @returns {boolean}
 */
function verify(token, sessionId, secret, ttl) {
    if (!token || typeof token !== "string")
        return false;
    if (!sessionId || typeof sessionId !== "string")
        return false;
    if (!secret || typeof secret !== "string")
        return false;

    const dotIdx = token.indexOf(".");
    if (dotIdx < 0)
        return false;

    const tsHex = token.substring(0, dotIdx);
    const mac = token.substring(dotIdx + 1);

    if (tsHex.length === 0 || mac.length === 0)
        return false;

    const expected = computeHmac(sessionId, tsHex, secret);

    // Constant-time compare in C (crypto.constantTimeEq), not the interpreter.
    if (!crypto.constantTimeEq(mac, expected))
        return false;

    const ts = parseInt(tsHex, 16);
    if (isNaN(ts))
        return false;

    const age = ttl !== undefined ? ttl : 3600;
    const now = time.now();
    if (now - ts > age)
        return false;
    if (ts > now + 60)
        return false;

    return true;
}

function parseCookieSessionId(req, cookieName) {
    const ck = req.header("Cookie");
    if (!ck) return null;
    const cookies = cookie.parse(ck);
    const val = cookies[cookieName];
    return val || null;
}

/**
 * Build a CSRF middleware. Register with `app.usePost()`.
 *
 * Safe methods (GET/HEAD/OPTIONS): generates a token, attaches to
 * `req.ctx.csrf_token`. Unsafe methods: verifies token from
 * `X-CSRF-Token` header or `_csrf` form field; sends 403 on failure.
 *
 * Body parsing capped at 1 MiB / 256 form pairs.
 *
 * @param {Object}  opts
 * @param {string}  opts.secret              **Required.** HMAC secret.
 * @param {string}  [opts.sessionKey="session_id"]
 * @param {number}  [opts.ttl=3600]  Max token age in seconds. Alias: `maxAge`.
 * @param {string[]} [opts.safeMethods=["GET","HEAD","OPTIONS"]]
 * @param {string}  [opts.headerName="x-csrf-token"]
 * @param {string}  [opts.fieldName="_csrf"]
 * @param {string}  [opts.name="hull_session"]  Session cookie name for the
 *   session-id fallback. Alias: `cookieName`.
 * @param {boolean} [opts.requireSession=false]  Reject unsafe requests with no
 *   resolvable session (403) instead of passing them through.
 * @returns {(req, res) => number}
 * @throws {Error} If `opts.secret` is missing.
 */
function middleware(opts) {
    const o = opts || {};
    const secret = o.secret;
    // Canonical `ttl`; back-compat alias `maxAge`.
    const ttl = o.ttl !== undefined ? o.ttl : (o.maxAge !== undefined ? o.maxAge : 3600);
    // Fallback session-cookie name when upstream session middleware hasn't
    // populated req.ctx[sessionKey]. Matches the session/auth middleware
    // default ("hull_session") so the fallback actually finds the cookie.
    // Canonical `name`; back-compat alias `cookieName`.
    const cookieName = o.name || o.cookieName || "hull_session";
    const sessionKey = o.sessionKey || "session_id";
    const headerName = o.headerName || "X-CSRF-Token";
    const fieldName = o.fieldName || "_csrf";
    const requireSession = o.requireSession || false;
    const safeMethods = { "GET": true, "HEAD": true, "OPTIONS": true };

    if (!secret)
        throw new Error("csrf.middleware requires opts.secret");
    // SECURITY: opts.secret should be a 32+ byte random key; a short secret is
    // brute-forceable. Not hard-rejected (back-compat), but strongly recommended.

    // Resolve session id with the same precedence as the Lua sibling:
    // prefer req.ctx[sessionKey] (set by upstream session middleware
    // on the SAME request), fall back to parsing the cookie.
    function resolveSessionId(req) {
        if (req.ctx && req.ctx[sessionKey]) return req.ctx[sessionKey];
        return parseCookieSessionId(req, cookieName);
    }

    return function(req, res) {
        if (safeMethods[req.method]) {
            // Generate token on safe methods for templates
            const sessionId = resolveSessionId(req);
            if (sessionId) {
                if (!req.ctx) req.ctx = {};
                req.ctx.csrf_token = generate(sessionId, secret);
            }
            return 0;
        }

        const sessionId = resolveSessionId(req);

        // CSRF only applies to authenticated sessions by default.
        // Set requireSession: true to reject unauthenticated unsafe requests.
        if (!sessionId) {
            if (requireSession) {
                res.status(403);
                res.json({ error: "csrf: session required for unsafe methods" });
                return 1;
            }
            return 0;
        }

        let token = req.header(headerName);
        if (!token && req.body) {
            const body = req.body;
            // Defense-in-depth: cap body length before split to avoid
            // splitting a multi-MB payload into a giant pairs array on
            // every unsafe-method request (M-5).
            //
            // Note (Phase 6 audit L-2): 1 MiB is the hard cap. Large
            // multipart uploads should use a multipart parser BEFORE
            // csrf.middleware in the stack so the body is already
            // pre-consumed by the time we get here. A url-encoded form
            // legitimately needing > 1 MiB is exceedingly rare; the cap
            // bounds worst-case work per request.
            if (body.length > 1 << 20) {  // 1 MiB
                res.status(413);
                res.json({ error: "CSRF: body too large" });
                return 1;
            }
            const pairs = body.split("&");
            // Parity with Lua sibling: cap pair count. A 1 MiB body
            // of `a=b&a=b&…` can produce 500k+ pairs; bounded N keeps
            // worst-case work flat. Auth flows submit a handful of
            // fields; 256 is generous.
            if (pairs.length > 256) {
                res.status(413);
                res.json({ error: "CSRF: too many form fields" });
                return 1;
            }
            for (let k = 0; k < pairs.length; k++) {
                // Parity with Lua sibling (MAX_PAIR_BYTES): skip any single
                // pair over 4 KiB before indexOf/decode. A CSRF token is short
                // by construction; without this bound a single ~1 MiB `_csrf`
                // pair (under the total-body cap) forces a full-body
                // decodeURIComponent on every unsafe request.
                if (pairs[k].length > 4096) continue;
                const eqIdx = pairs[k].indexOf("=");
                if (eqIdx >= 0) {
                    const key = pairs[k].substring(0, eqIdx);
                    if (key === fieldName) {
                        const raw = pairs[k].substring(eqIdx + 1);
                        try {
                            token = decodeURIComponent(raw);
                        } catch (_e) {
                            // Malformed percent-encoding (e.g. "%X") — fall
                            // back to the raw value rather than throwing
                            // URIError out of the middleware (H-1).
                            token = raw;
                        }
                        break;
                    }
                }
            }
        }

        if (!token) {
            res.status(403);
            res.json({ error: "CSRF token missing" });
            return 1;
        }

        if (!verify(token, sessionId, secret, ttl)) {
            res.status(403);
            res.json({ error: "CSRF token invalid" });
            return 1;
        }

        return 0;
    };
}

const csrf = { generate, verify, middleware };
export { csrf };
