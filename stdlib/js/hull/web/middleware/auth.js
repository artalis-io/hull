/**
 * @file hull:web:middleware:auth
 * @module hull:web:middleware:auth
 * @description Authentication middleware factories (session + JWT) and login/logout helpers.
 *
 * Lua parity: same surface as `hull.web.middleware.auth` with camelCase
 * option names.
 *
 * @example
 * import { auth } from "hull:web:middleware:auth";
 * import { session } from "hull:web:middleware:session";
 * session.init();
 *
 * // Cookie-based session auth for /app/*; redirect to /login on failure
 * app.use("*", "/app/*", auth.sessionMiddleware({ loginPath: "/login" }));
 *
 * // Bearer-token JWT auth for /api/*
 * app.use("*", "/api/*", auth.jwtMiddleware({ secret: env.get("JWT_SECRET") }));
 *
 * @license AGPL-3.0-or-later
 */

import { cookie } from "hull:web:cookie";
import { session } from "hull:web:middleware:session";
import { jwt } from "hull:jwt";
import { time } from "hull:time";

function parseCookieHeader(req) {
    const header = req.header("Cookie");
    if (!header) return {};
    return cookie.parse(header);
}

/**
 * Session-based authentication middleware.
 *
 * On match, populates `req.ctx.session_id` and `req.ctx.session` (the
 * session data object). On miss:
 *   - sends `401 {error}` (default), OR
 *   - redirects to `opts.loginPath` (302), OR
 *   - returns 0 if `opts.optional`.
 *
 * @param {Object} [opts]
 * @param {string} [opts.name="hull_session"]  Session cookie name (matches
 *   hull:web:middleware:session's `name`). Back-compat alias: `cookieName`.
 * @param {string} [opts.loginPath]    Redirect target on failure.
 * @param {boolean} [opts.optional=false]
 * @param {string[]} [opts.excludePaths]   Paths skipped entirely.
 * @returns {(req, res) => number}     Middleware function.
 */
function sessionMiddleware(opts) {
    const o = opts || {};
    const cookieName = o.name || o.cookieName || "hull_session";
    const loginPath = o.loginPath || null;
    const optional = o.optional === true;
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
            if (optional) return 0;
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
            if (optional) return 0;
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

        // Attach session data to request context for downstream handlers.
        // Use the canonical snake_case keys so the same handler code works
        // on Lua and JS, and so session.loginHandler's rotate path can read
        // req.ctx.session_id without re-parsing the cookie.
        if (!req.ctx) req.ctx = {};
        req.ctx.session_id = sessionId;
        req.ctx.session = data;

        return 0;
    };
}

/**
 * JWT Bearer-token authentication middleware.
 *
 * Reads `Authorization: Bearer <token>`, verifies via {@link
 * module:hull:jwt.verify}, and populates `req.ctx.user` with the decoded
 * payload on success. On failure sends `401 {error}` or returns 0 if
 * `opts.optional`. CSRF is not needed for Bearer auth.
 *
 * @param {Object}  opts
 * @param {string}  opts.secret    HMAC-SHA256 secret. **Required.**
 * @param {boolean} [opts.optional=false]
 * @returns {(req, res) => number}
 * @throws {Error} If `opts.secret` is missing (at factory time).
 */
function jwtMiddleware(opts) {
    const o = opts || {};
    const secret = o.secret;
    const optional = o.optional === true;
    const excludePaths = o.excludePaths || [];

    if (!secret)
        throw new Error("jwtMiddleware requires opts.secret");
    // SECURITY: opts.secret should be a 32+ byte random key; a short secret is
    // brute-forceable. Not hard-rejected (back-compat), but strongly recommended.

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
            if (optional) return 0;
            res.status(401);
            res.json({ error: "missing bearer token" });
            return 1;
        }

        const token = authHeader.substring(7);
        const result = jwt.verify(token, secret);

        // jwt.verify returns [payload, null] on success, [null, "reason"] on failure
        if (!result[0]) {
            if (optional) return 0;
            res.status(401);
            res.json({ error: result[1] || "invalid token" });
            return 1;
        }

        // Attach decoded payload to request context. Use req.ctx.user
        // (matching Lua's jwt_middleware) so handler code reads the same
        // key on both runtimes.
        if (!req.ctx) req.ctx = {};
        req.ctx.user = result[0];

        return 0;
    };
}

// Whitelisted cookie attributes — keep in sync with stdlib/js/hull/web/cookie.js
const COOKIE_ATTRS = ["path", "domain", "httpOnly", "secure", "sameSite",
                       "maxAge", "expires"];

function copyCookieOpts(src) {
    const o = Object.create(null);
    if (src) {
        for (const k of COOKIE_ATTRS) {
            if (Object.prototype.hasOwnProperty.call(src, k))
                o[k] = src[k];
        }
    }
    return o;
}

/**
 * Create a session and set the auth cookie.
 *
 * @param {Object} req            (Unused; reserved.)
 * @param {Object} res            Response — `Set-Cookie` is added here.
 * @param {Object} userData       Session payload, e.g. `{ user_id: 1 }`.
 * @param {Object} [opts]
 * @param {string} [opts.name="hull_session"]  Session cookie name (matches
 *   hull:web:middleware:session's `name`). Back-compat alias: `cookieName`.
 * @param {Object} [opts.cookieOpts]   Forwarded to {@link module:hull:web:cookie.serialize}.
 * @returns {string}              Session id (hex).
 */
function login(req, res, userData, opts) {
    const o = opts || {};
    const cookieName = o.name || o.cookieName || "hull_session";
    const cookieOpts = copyCookieOpts(o.cookieOpts);

    // Set Max-Age from session TTL if not explicitly provided
    if (cookieOpts.maxAge === undefined && o.ttl)
        cookieOpts.maxAge = o.ttl;

    const sessionId = session.create(userData || {});

    const setCookie = cookie.serialize(cookieName, sessionId, cookieOpts);
    res.header("Set-Cookie", setCookie);

    return sessionId;
}

/**
 * Destroy the current session and clear the auth cookie.
 *
 * @param {Object} req            Reads the session cookie.
 * @param {Object} res            Emits a `Set-Cookie` that clears it.
 * @param {Object} [opts]
 * @param {string} [opts.name="hull_session"]  Session cookie name (matches
 *   hull:web:middleware:session's `name`). Back-compat alias: `cookieName`.
 * @param {Object} [opts.cookieOpts]   Forwarded to {@link module:hull:web:cookie.clear}.
 */
function logout(req, res, opts) {
    const o = opts || {};
    const cookieName = o.name || o.cookieName || "hull_session";
    const cookieOpts = o.cookieOpts || {};

    const cookies = parseCookieHeader(req);
    const sessionId = cookies[cookieName];

    if (sessionId)
        session.destroy(sessionId);

    res.header("Set-Cookie", cookie.clear(cookieName, cookieOpts));
}

const auth = { sessionMiddleware, jwtMiddleware, login, logout };
export { auth };
