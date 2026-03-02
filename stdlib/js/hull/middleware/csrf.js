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
import { time } from "hull:time";

function secretToHex(secret) {
    let hex = "";
    for (let i = 0; i < secret.length; i++)
        hex += secret.charCodeAt(i).toString(16).padStart(2, "0");
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

function middleware(opts) {
    const o = opts || {};
    const secret = o.secret;
    const maxAge = o.maxAge !== undefined ? o.maxAge : 3600;
    const cookieName = o.cookieName || "hull.sid";
    const headerName = o.headerName || "X-CSRF-Token";
    const fieldName = o.fieldName || "_csrf";
    const safeMethods = { "GET": true, "HEAD": true, "OPTIONS": true };

    if (!secret)
        throw new Error("csrf.middleware requires opts.secret");

    return function(req, res) {
        if (safeMethods[req.method])
            return 0;

        var sessionId = null;
        var ck = req.header("Cookie");
        if (ck) {
            var parts = ck.split(";");
            for (var j = 0; j < parts.length; j++) {
                var p = parts[j].trim();
                var eq = p.indexOf("=");
                if (eq >= 0) {
                    var n = p.substring(0, eq).trim();
                    if (n === cookieName) {
                        sessionId = p.substring(eq + 1).trim();
                        break;
                    }
                }
            }
        }

        if (!sessionId)
            return 0;

        var token = req.header(headerName);
        if (!token && req.body) {
            var body = req.body;
            var pairs = body.split("&");
            for (var k = 0; k < pairs.length; k++) {
                var eqIdx = pairs[k].indexOf("=");
                if (eqIdx >= 0) {
                    var key = pairs[k].substring(0, eqIdx);
                    if (key === fieldName) {
                        token = pairs[k].substring(eqIdx + 1);
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
