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
    const maxAge = String(o.max_age !== undefined ? o.max_age : 86400);

    return function corsMiddleware(req, res) {
        const origin = req.headers.origin;
        if (!origin) return 0;

        if (!isAllowedOrigin(origin, origins)) return 0;

        res.header("Access-Control-Allow-Origin", origin);
        res.header("Access-Control-Allow-Methods", methods);
        res.header("Access-Control-Allow-Headers", headers);
        res.header("Access-Control-Max-Age", maxAge);

        if (credentials) {
            res.header("Access-Control-Allow-Credentials", "true");
        }

        // Handle preflight
        if (req.method === "OPTIONS") {
            res.status(204).text("");
            return 1;
        }

        return 0;
    };
}

const cors = { middleware, isAllowedOrigin };
export { cors };
