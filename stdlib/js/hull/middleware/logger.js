/*
 * hull:middleware:logger -- Request logging middleware (logfmt)
 *
 * Logs incoming requests as single-line key=value pairs (logfmt).
 * Sets X-Request-ID header for request tracing.
 *
 * logger.generateId()              - hex counter string for request ID
 * logger.formatLine(entries)       - key=value pairs -> single string
 * logger.shouldSkip(path, skip)    - exact-match path check
 * logger.middleware(opts)          - returns middleware function
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { log } from "hull:log";

let counter = 0;

function generateId() {
    counter++;
    return counter.toString(16);
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

function middleware(opts) {
    const o = opts || {};
    const skip = o.skip || null;
    const includeHeaders = o.include_headers || null;

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
