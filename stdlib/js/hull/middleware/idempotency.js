/*
 * hull:middleware:idempotency -- Idempotency-Key middleware for safe retries
 *
 * Prevents duplicate side effects when clients retry POST requests by caching
 * responses keyed by (principal_id, idempotency_key). Uses SHA-256 fingerprint
 * to detect key reuse with different request bodies (409 Conflict).
 *
 * Usage:
 *   import { idempotency } from "hull:middleware:idempotency";
 *   idempotency.init({ ttl: 86400 });
 *   app.usePost("POST", "/api/*", idempotency.middleware({
 *       getPrincipal: (req) => req.ctx?.session?.user_id || "__anon",
 *   }));
 *
 * Handlers capture responses via idempotency.respond(req, res, status, body):
 *   app.post("/api/events", (req, res) => {
 *       // ... do work inside db.batch() ...
 *       idempotency.respond(req, res, 201, { event_id: id });
 *   });
 *
 * On retry with same key+body: cached response returned, handler never runs.
 * On retry with same key+different body: 409 Conflict.
 * Without Idempotency-Key header: handler runs normally (no caching).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { db } from "hull:db";
import { crypto } from "hull:crypto";
import { time } from "hull:time";
import { json } from "hull:json";

let idemTtl = 86400;
const HEADER_NAME = "idempotency-key";

/**
 * Initialize the idempotency table.
 * @param {Object} opts - Options: ttl (seconds, default 86400)
 */
function init(opts) {
    const o = opts || {};
    if (o.ttl !== undefined) idemTtl = o.ttl;

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_idempotency_keys (" +
        "  key            TEXT NOT NULL," +
        "  principal_id   TEXT NOT NULL DEFAULT '__anon'," +
        "  fingerprint    TEXT NOT NULL," +
        "  endpoint       TEXT NOT NULL," +
        "  status         INTEGER," +
        "  response_body  TEXT," +
        "  response_headers TEXT," +
        "  state          TEXT NOT NULL DEFAULT 'inflight'," +
        "  created_at     INTEGER NOT NULL," +
        "  expires_at     INTEGER NOT NULL," +
        "  PRIMARY KEY (principal_id, key)" +
        ")"
    );
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_idem_expires " +
        "ON _hull_idempotency_keys(expires_at)"
    );
}

/**
 * Compute a request fingerprint: SHA-256(method + path + body).
 */
function computeFingerprint(req) {
    const data = (req.method || "") + "\0" + (req.path || "") + "\0" + (req.body || "");
    return crypto.sha256(data);
}

/**
 * Create a post-body middleware for idempotency-key processing.
 * @param {Object} opts
 * @param {Function} opts.getPrincipal - (req) => string (default: session user_id or "__anon")
 * @param {number} opts.ttl - override module TTL
 * @param {string} opts.headerName - header name (default "idempotency-key")
 * @param {string[]} opts.methods - methods to intercept (default ["POST"])
 */
function middleware(opts) {
    const o = opts || {};

    const getPrincipal = o.getPrincipal || function(req) {
        if (req.ctx && req.ctx.session && req.ctx.session.user_id)
            return String(req.ctx.session.user_id);
        return "__anon";
    };

    const ttl = o.ttl !== undefined ? o.ttl : idemTtl;
    const headerName = o.headerName || HEADER_NAME;

    const methodList = o.methods || ["POST"];
    const methods = {};
    for (let i = 0; i < methodList.length; i++)
        methods[methodList[i]] = true;

    // Counter for probabilistic cleanup (1 in 100 requests)
    let requestCount = 0;

    return function(req, res) {
        if (!methods[req.method])
            return 0;

        const key = req.header(headerName);
        if (!key || key === "")
            return 0;

        if (!req.ctx) req.ctx = {};

        const principalId = getPrincipal(req);
        const fingerprint = computeFingerprint(req);
        const endpoint = req.method + " " + req.path;
        const now = time.now();

        // Probabilistic cleanup
        requestCount++;
        if (requestCount % 100 === 0)
            db.exec("DELETE FROM _hull_idempotency_keys WHERE expires_at <= ?", [now]);

        // Check for existing key
        const rows = db.query(
            "SELECT fingerprint, state, status, response_body, response_headers, expires_at " +
            "FROM _hull_idempotency_keys WHERE principal_id = ? AND key = ?",
            [principalId, key]
        );

        if (rows && rows.length > 0) {
            const row = rows[0];

            if (row.expires_at <= now) {
                // Expired: delete and treat as new
                db.exec(
                    "DELETE FROM _hull_idempotency_keys WHERE principal_id = ? AND key = ?",
                    [principalId, key]
                );
            } else {
                // Fingerprint mismatch
                if (row.fingerprint !== fingerprint) {
                    res.status(409);
                    res.json({ error: "idempotency key already used with different request body" });
                    return 1;
                }

                // Still in-flight
                if (row.state === "inflight") {
                    res.status(409);
                    res.json({ error: "request with this idempotency key is already in progress" });
                    return 1;
                }

                // Completed with cached response
                if (row.state === "complete" && row.status) {
                    res.status(row.status);
                    if (row.response_headers) {
                        const headers = json.decode(row.response_headers);
                        if (headers) {
                            const keys = Object.keys(headers);
                            for (let i = 0; i < keys.length; i++)
                                res.header(keys[i], headers[keys[i]]);
                        }
                    }
                    res.header("X-Idempotency-Replay", "true");
                    if (row.response_body) {
                        res.header("Content-Type", "application/json");
                        res.text(row.response_body);
                    } else {
                        res.text("");
                    }
                    return 1;
                }

                // Complete but no cached response: delete and re-run
                db.exec(
                    "DELETE FROM _hull_idempotency_keys WHERE principal_id = ? AND key = ?",
                    [principalId, key]
                );
            }
        }

        // Insert in-flight record
        db.exec(
            "INSERT INTO _hull_idempotency_keys " +
            "(key, principal_id, fingerprint, endpoint, state, created_at, expires_at) " +
            "VALUES (?, ?, ?, ?, 'inflight', ?, ?)",
            [key, principalId, fingerprint, endpoint, now, now + ttl]
        );

        // Store key info in context for respond() to use
        req.ctx._idem_key = key;
        req.ctx._idem_principal = principalId;

        return 0;
    };
}

/**
 * Send a response and cache it for idempotency replay.
 * Must be called from within a handler that has an idempotency key active.
 *
 * @param {Object} req - Request object
 * @param {Object} res - Response object
 * @param {number} statusCode - HTTP status code
 * @param {Object} data - Response body (will be JSON-encoded)
 * @param {Object} extraHeaders - Optional additional headers
 */
function respond(req, res, statusCode, data, extraHeaders) {
    const bodyStr = json.encode(data);

    res.status(statusCode);
    if (extraHeaders) {
        const keys = Object.keys(extraHeaders);
        for (let i = 0; i < keys.length; i++)
            res.header(keys[i], extraHeaders[keys[i]]);
    }
    res.json(data);

    // Cache if idempotency key is active
    if (req.ctx && req.ctx._idem_key) {
        let headersStr = null;
        if (extraHeaders)
            headersStr = json.encode(extraHeaders);

        db.exec(
            "UPDATE _hull_idempotency_keys SET state = 'complete', status = ?, " +
            "response_body = ?, response_headers = ? WHERE principal_id = ? AND key = ?",
            [statusCode, bodyStr, headersStr, req.ctx._idem_principal, req.ctx._idem_key]
        );
    }
}

/**
 * Mark an idempotency key as complete without caching a response.
 * Retries will re-execute the handler (prevents concurrent duplicates only).
 */
function complete(req) {
    if (req.ctx && req.ctx._idem_key) {
        db.exec(
            "UPDATE _hull_idempotency_keys SET state = 'complete' " +
            "WHERE principal_id = ? AND key = ?",
            [req.ctx._idem_principal, req.ctx._idem_key]
        );
    }
}

/**
 * Delete expired idempotency keys. Returns number of deleted rows.
 */
function cleanup() {
    const now = time.now();
    return db.exec("DELETE FROM _hull_idempotency_keys WHERE expires_at <= ?", [now]);
}

const idempotency = { init, middleware, respond, complete, cleanup };
export { idempotency };
