/*
 * hull:cors -- CORS middleware factory
 *
 * cors.middleware(opts)                     - returns middleware function
 * cors.isAllowedOrigin(origin, origins)     - pure helper, testable
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

function isAllowedOrigin(origin, origins) {
    if (!origin || !origins) return false;
    for (let i = 0; i < origins.length; i++) {
        if (origins[i] === "*" || origins[i] === origin) return true;
    }
    return false;
}

function middleware(opts) {
    const o = opts || {};

    const origins = o.origins || ["*"];
    const methods = o.methods || "GET, POST, PUT, DELETE, OPTIONS";
    const headers = o.headers || "Content-Type, Authorization";
    const credentials = o.credentials || false;
    const rawMaxAge = o.maxAge !== undefined ? o.maxAge : o.max_age;
    const maxAge = String(rawMaxAge !== undefined ? rawMaxAge : 86400);

    // M-1: refuse the unsafe combination `credentials: true` + wildcard
    // origin. Browsers would also reject it, but failing fast at factory
    // time surfaces the misconfiguration in dev rather than silently
    // serving an insecure CORS policy.
    if (credentials && origins.indexOf("*") !== -1) {
        throw new Error(
            "cors: credentials:true is incompatible with origins:['*']; " +
            "list explicit origins."
        );
    }

    return function corsMiddleware(req, res) {
        const origin = req.header("Origin");
        if (!origin) return 0;

        if (!isAllowedOrigin(origin, origins)) return 0;

        res.header("Access-Control-Allow-Origin", origin);
        res.header("Vary", "Origin");

        if (credentials) {
            res.header("Access-Control-Allow-Credentials", "true");
        }

        // Preflight: send method/header/max-age headers + 204
        if (req.method === "OPTIONS") {
            res.header("Access-Control-Allow-Methods", methods);
            res.header("Access-Control-Allow-Headers", headers);
            res.header("Access-Control-Max-Age", maxAge);
            res.status(204).text("");
            return 1;
        }

        return 0;
    };
}

const cors = { middleware, isAllowedOrigin };
export { cors };
