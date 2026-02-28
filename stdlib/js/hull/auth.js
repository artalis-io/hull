/*
 * hull:auth -- Authentication middleware factories
 *
 * auth.sessionMiddleware(opts) - returns middleware for session-based auth
 * auth.jwtMiddleware(opts)     - returns middleware for JWT bearer auth
 * auth.login(req, res, userData, opts)  - creates session, sets cookie, returns sessionId
 * auth.logout(req, res, opts)           - destroys session, clears cookie
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { cookie } from "hull:cookie";
import { session } from "hull:session";
import { jwt } from "hull:jwt";
import { time } from "hull:time";

function parseCookieHeader(req) {
    const header = req.header("Cookie");
    if (!header) return {};
    return cookie.parse(header);
}

function sessionMiddleware(opts) {
    const o = opts || {};
    const cookieName = o.cookieName || "hull.sid";
    const loginPath = o.loginPath || null;
    const excludePaths = o.excludePaths || [];

    return function sessionMw(req, res) {
        // Check if path is excluded
        const path = req.path || "";
        for (let i = 0; i < excludePaths.length; i++) {
            const ep = excludePaths[i];
            if (ep.endsWith("/*")) {
                if (path.startsWith(ep.substring(0, ep.length - 1)))
                    return 0;
            } else if (path === ep) {
                return 0;
            }
        }

        const cookies = parseCookieHeader(req);
        const sessionId = cookies[cookieName];

        if (!sessionId) {
            if (loginPath) {
                res.status(302);
                res.header("Location", loginPath);
                res.body("");
                return 1;
            }
            res.status(401);
            res.json({ error: "authentication required" });
            return 1;
        }

        const data = session.load(sessionId);
        if (!data) {
            // Session expired or invalid -- clear the stale cookie
            res.header("Set-Cookie", cookie.clear(cookieName, o.cookieOpts));
            if (loginPath) {
                res.status(302);
                res.header("Location", loginPath);
                res.body("");
                return 1;
            }
            res.status(401);
            res.json({ error: "session expired" });
            return 1;
        }

        // Attach session data to request context for downstream handlers
        req.ctx = {
            sessionId: sessionId,
            session: data
        };

        return 0;
    };
}

function jwtMiddleware(opts) {
    const o = opts || {};
    const secret = o.secret;
    const excludePaths = o.excludePaths || [];

    if (!secret)
        throw new Error("jwtMiddleware requires opts.secret");

    return function jwtMw(req, res) {
        // Check if path is excluded
        const path = req.path || "";
        for (let i = 0; i < excludePaths.length; i++) {
            const ep = excludePaths[i];
            if (ep.endsWith("/*")) {
                if (path.startsWith(ep.substring(0, ep.length - 1)))
                    return 0;
            } else if (path === ep) {
                return 0;
            }
        }

        // Extract token from Authorization header
        const authHeader = req.header("Authorization");
        if (!authHeader || !authHeader.startsWith("Bearer ")) {
            res.status(401);
            res.json({ error: "missing bearer token" });
            return 1;
        }

        const token = authHeader.substring(7);
        const result = jwt.verify(token, secret);

        // jwt.verify returns [null, "reason"] on failure
        if (Array.isArray(result)) {
            res.status(401);
            res.json({ error: result[1] || "invalid token" });
            return 1;
        }

        // Attach decoded payload to request context
        req.ctx = {
            token: token,
            claims: result
        };

        return 0;
    };
}

function login(req, res, userData, opts) {
    const o = opts || {};
    const cookieName = o.cookieName || "hull.sid";
    const cookieOpts = Object.assign({}, o.cookieOpts || {});

    // Set Max-Age from session TTL if not explicitly provided
    if (cookieOpts.maxAge === undefined && o.ttl)
        cookieOpts.maxAge = o.ttl;

    const sessionId = session.create(userData || {});

    const setCookie = cookie.serialize(cookieName, sessionId, cookieOpts);
    res.header("Set-Cookie", setCookie);

    return sessionId;
}

function logout(req, res, opts) {
    const o = opts || {};
    const cookieName = o.cookieName || "hull.sid";
    const cookieOpts = o.cookieOpts || {};

    const cookies = parseCookieHeader(req);
    const sessionId = cookies[cookieName];

    if (sessionId)
        session.destroy(sessionId);

    res.header("Set-Cookie", cookie.clear(cookieName, cookieOpts));
}

const auth = { sessionMiddleware, jwtMiddleware, login, logout };
export { auth };
