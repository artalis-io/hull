/**
 * @file hull:middleware:auth
 * @module hull:middleware:auth
 * @description Authentication middleware factories (session + JWT) and login/logout helpers.
 *
 * Lua parity: same surface as `hull.middleware.auth` with camelCase
 * option names.
 *
 * @example
 * import { auth } from "hull:middleware:auth";
 * import { session } from "hull:middleware:session";
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

import { cookie } from "hull:cookie";
import { session } from "hull:middleware:session";
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
 * On match, populates `req.ctx.sessionId` and `req.ctx.session` (the
 * session data object). On miss:
 *   - sends `401 {error}` (default), OR
 *   - redirects to `opts.loginPath` (302), OR
 *   - returns 0 if `opts.optional`.
 *
 * @param {Object} [opts]
 * @param {string} [opts.cookieName="hull_session"]
 * @param {string} [opts.loginPath]    Redirect target on failure.
 * @param {boolean} [opts.optional=false]
 * @param {string[]} [opts.excludePaths]   Paths skipped entirely.
 * @returns {(req, res) => number}     Middleware function.
 */
function sessionMiddleware(opts) {
    const o = opts || {};
    const cookieName = o.cookieName || "hull_session";
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
        if (!req.ctx) req.ctx = {};
        req.ctx.sessionId = sessionId;
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

        // jwt.verify returns [payload, null] on success, [null, "reason"] on failure
        if (!result[0]) {
            res.status(401);
            res.json({ error: result[1] || "invalid token" });
            return 1;
        }

        // Attach decoded payload to request context
        if (!req.ctx) req.ctx = {};
        req.ctx.token = token;
        req.ctx.claims = result[0];

        return 0;
    };
}

// Whitelisted cookie attributes — keep in sync with stdlib/js/hull/cookie.js
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
 * @param {string} [opts.cookieName="hull_session"]
 * @param {Object} [opts.cookieOpts]   Forwarded to {@link module:hull:cookie.serialize}.
 * @returns {string}              Session id (hex).
 */
function login(req, res, userData, opts) {
    const o = opts || {};
    const cookieName = o.cookieName || "hull_session";
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
 * @param {string} [opts.cookieName="hull_session"]
 * @param {Object} [opts.cookieOpts]   Forwarded to {@link module:hull:cookie.clear}.
 */
function logout(req, res, opts) {
    const o = opts || {};
    const cookieName = o.cookieName || "hull_session";
    const cookieOpts = o.cookieOpts || {};

    const cookies = parseCookieHeader(req);
    const sessionId = cookies[cookieName];

    if (sessionId)
        session.destroy(sessionId);

    res.header("Set-Cookie", cookie.clear(cookieName, cookieOpts));
}

const auth = { sessionMiddleware, jwtMiddleware, login, logout };
export { auth };
