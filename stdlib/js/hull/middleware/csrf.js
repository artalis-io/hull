/*
 * hull:csrf -- Stateless CSRF token generation and verification
 *
 * csrf.generate(sessionId, secret)              - returns "hexTimestamp.hmacHex"
 * csrf.verify(token, sessionId, secret, maxAge) - boolean
 * csrf.middleware(opts)                          - returns middleware function
 *
 * Tokens are stateless: HMAC(sessionId + "." + timestamp, secret).
 * No database required.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { crypto } from "hull:crypto";
import { cookie } from "hull:cookie";
import { time } from "hull:time";

// Secret must be ASCII-only; charCodeAt returns 16-bit code units
// for non-ASCII which would produce inconsistent HMAC keys.
function secretToHex(secret) {
    let hex = "";
    for (let i = 0; i < secret.length; i++) {
        const c = secret.charCodeAt(i);
        if (c > 127) throw new Error("secret must be ASCII-only");
        hex += c.toString(16).padStart(2, "0");
    }
    return hex;
}

function computeHmac(sessionId, timestamp, secret) {
    const msg = sessionId + "." + timestamp;
    const keyHex = secretToHex(secret);
    return crypto.hmacSha256(msg, keyHex);
}

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

function verify(token, sessionId, secret, maxAge) {
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

    if (mac.length !== expected.length)
        return false;
    let diff = 0;
    for (let i = 0; i < mac.length; i++)
        diff |= mac.charCodeAt(i) ^ expected.charCodeAt(i);
    if (diff !== 0)
        return false;

    const ts = parseInt(tsHex, 16);
    if (isNaN(ts))
        return false;

    const age = maxAge !== undefined ? maxAge : 3600;
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

function middleware(opts) {
    const o = opts || {};
    const secret = o.secret;
    const maxAge = o.maxAge !== undefined ? o.maxAge : 3600;
    const cookieName = o.cookieName || "hull.sid";
    const headerName = o.headerName || "X-CSRF-Token";
    const fieldName = o.fieldName || "_csrf";
    const requireSession = o.requireSession || false;
    const safeMethods = { "GET": true, "HEAD": true, "OPTIONS": true };

    if (!secret)
        throw new Error("csrf.middleware requires opts.secret");

    return function(req, res) {
        if (safeMethods[req.method]) {
            // Generate token on safe methods for templates
            const sessionId = parseCookieSessionId(req, cookieName);
            if (sessionId) {
                if (!req.ctx) req.ctx = {};
                req.ctx.csrf_token = generate(sessionId, secret);
            }
            return 0;
        }

        const sessionId = parseCookieSessionId(req, cookieName);

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
            if (body.length > 1 << 20) {  // 1 MiB
                res.status(413);
                res.json({ error: "CSRF: body too large" });
                return 1;
            }
            const pairs = body.split("&");
            for (let k = 0; k < pairs.length; k++) {
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

        if (!verify(token, sessionId, secret, maxAge)) {
            res.status(403);
            res.json({ error: "CSRF token invalid" });
            return 1;
        }

        return 0;
    };
}

const csrf = { generate, verify, middleware };
export { csrf };
