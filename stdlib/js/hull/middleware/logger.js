/**
 * @file hull:middleware:logger
 * @module hull:middleware:logger
 * @description Request logging middleware (logfmt output). Lua parity:
 *   `hull.middleware.logger`.
 *
 * Emits one logfmt line per request with method/path/status/duration.
 * Assigns a request id (`req.ctx.requestId`) and sets `X-Request-ID`
 * on the response.
 *
 * @license AGPL-3.0-or-later
 */

import { log } from "hull:log";

let counter = 0;

function generateId() {
    counter++;
    if (counter > 0xffffffffffff) counter = 1;
    return counter.toString(16).padStart(8, "0");
}

function sanitizeValue(v) {
    return v.replace(/\\/g, "\\\\").replace(/\n/g, "\\n").replace(/\r/g, "\\r").replace(/"/g, '\\"');
}

function needsQuoting(v) {
    return v.indexOf(" ") >= 0 || v.indexOf("=") >= 0 || v.indexOf('"') >= 0 || v.indexOf("\n") >= 0 || v.indexOf("\r") >= 0;
}

function formatLine(entries) {
    const parts = [];
    for (let i = 0; i < entries.length; i++) {
        const k = entries[i][0];
        const raw = String(entries[i][1]);
        const v = sanitizeValue(raw);
        if (needsQuoting(raw)) {
            parts.push(k + '="' + v + '"');
        } else {
            parts.push(k + "=" + v);
        }
    }
    return parts.join(" ");
}

function shouldSkip(path, skipList) {
    if (!skipList) return false;
    for (let i = 0; i < skipList.length; i++) {
        if (skipList[i] === path) return true;
    }
    return false;
}

/**
 * Build the logger middleware.
 *
 * @param {Object}    [opts]
 * @param {string[]}  [opts.skip]              Paths to skip (exact match).
 * @param {string[]}  [opts.includeHeaders]    Header names to include.
 * @param {string[]}  [opts.include_headers]   Lua-parity alias.
 * @returns {(req, res) => number}
 *
 * @example
 * app.use("*", "/*", logger.middleware({ skip: ["/health"] }));
 */
function middleware(opts) {
    const o = opts || {};
    const skip = o.skip || null;
    // L-2: accept camelCase (idiomatic JS) and snake_case (Lua-parity).
    const includeHeaders = o.includeHeaders || o.include_headers || null;

    return function loggerMiddleware(req, res) {
        if (shouldSkip(req.path, skip))
            return 0;

        const reqId = generateId();
        if (!req.ctx) req.ctx = {};
        req.ctx.request_id = reqId;
        res.header("X-Request-ID", reqId);

        const entries = [
            ["method", req.method],
            ["path", req.path],
            ["req_id", reqId],
            ["body_in", req.content_length || 0],
        ];

        if (includeHeaders) {
            for (let i = 0; i < includeHeaders.length; i++) {
                const name = includeHeaders[i];
                const val = req.headers[name.toLowerCase()];
                if (val) {
                    entries.push([name.toLowerCase().replace(/-/g, "_"), val]);
                }
            }
        }

        log.info("req " + formatLine(entries));

        return 0;
    };
}

const logger = { generateId, formatLine, shouldSkip, middleware };
export { logger };
