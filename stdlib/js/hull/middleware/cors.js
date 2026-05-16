/**
 * @file hull:middleware:cors
 * @module hull:middleware:cors
 * @description CORS middleware factory.
 *
 * Lua parity: same surface as `hull.middleware.cors` with camelCase
 * options.
 *
 * @license AGPL-3.0-or-later
 */

/**
 * Check whether an origin is in the allowed list.
 *
 * @param {string} origin       Incoming `Origin` header value.
 * @param {string[]} origins    Allowed origins. Use `"*"` for any (browser-rejected
 *                              when combined with credentials — the factory throws).
 * @returns {boolean}
 */
function isAllowedOrigin(origin, origins) {
    if (!origin || !origins) return false;
    for (let i = 0; i < origins.length; i++) {
        if (origins[i] === "*" || origins[i] === origin) return true;
    }
    return false;
}

/**
 * Build a CORS middleware function for `app.use()`.
 *
 * The middleware skips requests without an `Origin` header, rejects
 * disallowed origins silently (browser blocks), and on allowed origins
 * emits `Access-Control-Allow-Origin` + `Vary` + (when `credentials`)
 * `Access-Control-Allow-Credentials`. On `OPTIONS` preflight: emits
 * methods/headers/max-age and replies 204 (short-circuit, returns 1).
 *
 * Fail-fast: throws at factory time if `credentials: true` is combined
 * with `origins: ["*"]` — browsers reject that combination and it's a
 * common bug.
 *
 * @param {Object} [opts]
 * @param {string[]} [opts.origins=["*"]]
 * @param {string}   [opts.methods="GET, POST, PUT, DELETE, OPTIONS"]
 * @param {string}   [opts.headers="Content-Type, Authorization"]
 * @param {boolean}  [opts.credentials=false]
 * @param {number}   [opts.maxAge=86400]
 * @returns {(req, res) => number}     Middleware function.
 * @throws {Error} If `credentials: true && origins: ["*"]`.
 *
 * @example
 * app.use("*", "/api/*", cors.middleware({
 *     origins: ["https://app.example.com"],
 *     credentials: true,
 * }));
 */
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
