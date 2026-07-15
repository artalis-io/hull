/**
 * @file hull:web:middleware:idempotency
 * @module hull:web:middleware:idempotency
 * @description Idempotency-Key middleware for safe POST retries. Lua parity:
 *   `hull.web.middleware.idempotency`.
 *
 * Prevents duplicate side effects when clients retry the same request by
 * caching the response keyed by `(principal_id, idempotency_key)`. A
 * `SHA-256(method || path || body)` fingerprint detects key reuse with a
 * different body (returns `409 Conflict`).
 *
 * **Replay semantics:**
 *   - Same key, same body → cached response returned (handler skipped).
 *   - Same key, different body → `409`.
 *   - Same key, still inflight → `409` (`request already in progress`).
 *   - No `Idempotency-Key` header → handler runs normally (no caching).
 *
 * **Header allowlist:** Only safe response headers are persisted/replayed.
 * `Set-Cookie`, `Authorization`, `Cookie`, and `X-*` credential patterns
 * (`x-auth`, `x-api-key`, …) are never cached, so a stored replay can't
 * outlive a revoked session.
 *
 * @license AGPL-3.0-or-later
 */

import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { crypto } from "hull:crypto";
import { time } from "hull:time";
import { json } from "hull:json";

let idemTtl = 86400;
const HEADER_NAME = "idempotency-key";

/* M-7 (Phase 5) + Phase 6 audit M-1: allowlist of headers safe to replay
 * or cache from a previous response. Excludes credential-bearing /
 * session-binding headers; explicitly allowlists common security
 * response headers (HSTS/CSP/etc.) so they aren't silently dropped on
 * replay. NO blanket X-* — the previous version was too permissive
 * (would replay X-Auth-Token, X-API-Key, X-CSRF-Token, etc.). */
const REPLAYABLE_HEADERS = {
    "content-type": 1,
    "content-language": 1,
    "content-encoding": 1,
    "location": 1,
    "etag": 1,
    "last-modified": 1,
    "cache-control": 1,
    "vary": 1,
    // Safe X-* (stdlib-emitted or widely-used non-credential):
    "x-request-id": 1,
    "x-ratelimit-limit": 1,
    "x-ratelimit-remaining": 1,
    "x-ratelimit-reset": 1,
    "x-idempotency-replay": 1,
    "x-content-type-options": 1,
    "x-frame-options": 1,
    // Other safe response-shaping headers:
    "strict-transport-security": 1,
    "content-security-policy": 1,
    "referrer-policy": 1,
    "permissions-policy": 1,
};

// X-* substrings we explicitly deny even if a future allowlist entry
// would catch them by name. Belt and braces.
const X_CREDENTIAL_PATTERNS = [
    "x-auth", "x-api-key", "x-csrf", "x-token",
    "x-forwarded-authorization", "x-amz-security-token",
    "x-aws-", "x-google-", "x-vault-", "x-jwt-",
];

function isReplayableHeader(name) {
    if (typeof name !== "string" || !name) return false;
    const lc = name.toLowerCase();
    // Always-deny credential headers (run FIRST so an accidental
    // allowlist addition can't override the deny — defense in depth
    // that actually defends; the previous order made the deny loop
    // dead code because the function fell through to `return false`).
    if (lc === "set-cookie" || lc === "authorization" ||
        lc === "cookie" || lc === "proxy-authenticate" ||
        lc === "www-authenticate") return false;
    for (let i = 0; i < X_CREDENTIAL_PATTERNS.length; i++) {
        if (lc.indexOf(X_CREDENTIAL_PATTERNS[i]) !== -1) return false;
    }
    // Then check allowlist.
    if (REPLAYABLE_HEADERS[lc]) return true;
    // Anything not on the allowlist is denied (no blanket X-*).
    return false;
}

/**
 * Initialize the `_hull_idempotency_keys` SQLite table. Idempotent.
 *
 * @param {Object} [opts]
 * @param {number} [opts.ttl=86400]  Key lifetime in seconds.
 */
function init(opts) {
    const o = opts || {};
    if (o.ttl !== undefined) idemTtl = o.ttl;

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_idempotency_keys (" +
        "  key            VARCHAR(255) NOT NULL," +
        "  principal_id   VARCHAR(255) NOT NULL DEFAULT '__anon'," +
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
 * Build a post-body idempotency middleware.
 *
 * Reads the key from `req.header(headerName)`. Same key+body → cached
 * replay; different body → 409; missing header → handler runs.
 *
 * @param {Object} [opts]
 * @param {(req) => string} [opts.getPrincipal]
 *   Returns a stable per-user key for scoping. Default:
 *   `req.ctx.session.user_id` or `"__anon"`.
 * @param {number}   [opts.ttl]        Override module TTL for this instance.
 * @param {string}   [opts.headerName="idempotency-key"]
 * @param {string[]} [opts.methods=["POST"]]
 * @returns {(req, res) => number}
 *
 * @example
 * app.usePost("POST", "/api/*", idempotency.middleware({
 *     getPrincipal: (req) => req.ctx?.session?.user_id || "__anon",
 * }));
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

    // Counter for probabilistic cleanup (every 100 requests)
    let requestCount = 0;
    const CLEANUP_INTERVAL = 100;

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

        // Periodic cleanup
        requestCount++;
        if (requestCount >= CLEANUP_INTERVAL) {
            requestCount = 0;
            db.exec("DELETE FROM _hull_idempotency_keys WHERE expires_at <= ?", [now]);
        }

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
                // Fingerprint mismatch. Constant-time comparison even though
                // fingerprints are SHA-256 of public inputs (method+path+body),
                // for consistency with jwt.js / csrf.js.
                // Hoist `rfp` so the `charCodeAt` call below can't read off a
                // (theoretically) null row.fingerprint — schema has NOT NULL
                // so it's defensive, but explicit > implicit (audit J#3).
                const rfp = row.fingerprint || "";
                let diff = rfp.length === fingerprint.length ? 0 : 1;
                for (let i = 0; i < fingerprint.length && i < rfp.length; i++)
                    diff |= rfp.charCodeAt(i) ^ fingerprint.charCodeAt(i);
                if (diff !== 0) {
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
                    // Decode once. Walk once. M-7 filtering AND
                    // Content-Type detection both happen in the
                    // single pass so a corrupted blob can't throw
                    // an uncaught SyntaxError from a second decode
                    // path (audit J#1).
                    let replayedCt = null;
                    if (row.response_headers) {
                        let headers;
                        try { headers = json.decode(row.response_headers); }
                        catch (_e) { headers = null; }
                        if (headers && typeof headers === "object") {
                            const keys = Object.keys(headers);
                            for (let i = 0; i < keys.length; i++) {
                                const k = keys[i];
                                const v = headers[k];
                                // M-7: defense-in-depth — drop credential-
                                // affecting headers from a replayed response
                                // (stale Set-Cookie / Authorization could
                                // outlive a revoked session) and reject any
                                // CRLF/NUL injection attempt.
                                if (!isReplayableHeader(k)) continue;
                                if (typeof v !== "string") continue;
                                if (/[\r\n\x00]/.test(v)) continue;
                                res.header(k, v);
                                if (k.toLowerCase() === "content-type") {
                                    replayedCt = v;
                                }
                            }
                        }
                    }
                    res.header("X-Idempotency-Replay", "true");
                    if (row.response_body) {
                        // Default to application/json for back-compat
                        // with old cached rows from before respondHtml()
                        // existed; respond()/respondHtml() always stash
                        // a Content-Type now so this branch is rare.
                        if (!replayedCt) {
                            res.header("Content-Type", "application/json");
                        }
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

        // Insert in-flight record (insert-if-absent guards the race with
        // a concurrent request claiming the same key).
        const inserted = db.insertIfAbsent(
            "_hull_idempotency_keys",
            ["principal_id", "key"],
            ["key", "principal_id", "fingerprint", "endpoint",
             "state", "created_at", "expires_at"],
            [key, principalId, fingerprint, endpoint,
             "inflight", now, now + ttl]
        );
        if (inserted === 0) {
            res.status(409);
            res.json({ error: "request with this idempotency key is already in progress" });
            return 1;
        }

        // Store key info in context for respond() to use
        req.ctx._idem_key = key;
        req.ctx._idem_principal = principalId;

        return 0;
    };
}

/**
 * Send a JSON response and cache it for idempotency replay.
 *
 * Call from inside a handler that has an idempotency key active.
 * `extraHeaders` is filtered through the same allowlist as the replay
 * path, so credential headers never persist on disk.
 *
 * @param {Object}   req
 * @param {Object}   res
 * @param {number}   statusCode  HTTP status to send + cache.
 * @param {Object}   data        Body data (JSON-encoded).
 * @param {Object<string,string>} [extraHeaders]  Optional response headers.
 *
 * @example
 * idempotency.respond(req, res, 201, { event_id: 42 });
 */
// Shared cache-write path. `bodyStr` is the literal response bytes;
// `contentType` is folded into the cached headers blob so the replay
// path serves the response with the correct mime.
function cacheAndSend(req, res, statusCode, bodyStr, contentType, extraHeaders, sendFn) {
    res.status(statusCode);
    if (extraHeaders) {
        const keys = Object.keys(extraHeaders);
        for (let i = 0; i < keys.length; i++) {
            const k = keys[i];
            const v = extraHeaders[k];
            if (!isReplayableHeader(k)) continue;
            if (typeof v !== "string") continue;
            if (/[\r\n\x00]/.test(v)) continue;
            res.header(k, v);
        }
    }
    sendFn(res, bodyStr);

    if (req.ctx && req.ctx._idem_key) {
        // Phase 6 audit M-3: filter headers through the allowlist
        // before writing to SQLite so credential headers never
        // persist on disk for the TTL.
        //
        // All keys normalised to lowercase before stash. The replay
        // path matches case-insensitively on read, so a caller passing
        // `Content-Type` (mixed) and our own `content-type` (lower)
        // would otherwise both land in the blob and emit two headers
        // on replay (audit J#2).
        const filtered = { "content-type": contentType };
        if (extraHeaders) {
            const keys = Object.keys(extraHeaders);
            for (let i = 0; i < keys.length; i++) {
                const k = keys[i];
                const v = extraHeaders[k];
                if (!isReplayableHeader(k)) continue;
                if (typeof v !== "string") continue;
                if (/[\r\n\x00]/.test(v)) continue;
                filtered[k.toLowerCase()] = v;
            }
        }
        const headersStr = json.encode(filtered);
        db.exec(
            "UPDATE _hull_idempotency_keys SET state = 'complete', status = ?, " +
            "response_body = ?, response_headers = ? WHERE principal_id = ? AND key = ?",
            [statusCode, bodyStr, headersStr, req.ctx._idem_principal, req.ctx._idem_key]
        );
    }
}

function respond(req, res, statusCode, data, extraHeaders) {
    cacheAndSend(req, res, statusCode,
        json.encode(data), "application/json",
        extraHeaders,
        (r, _) => r.json(data));
}

/**
 * Send an HTML response and cache it for idempotency replay.
 *
 * HTMX form retries against the same `Idempotency-Key` get the cached
 * HTML fragment back verbatim, with `Content-Type: text/html;
 * charset=utf-8`. Without this helper the replay path would serve the
 * cached body as JSON.
 *
 * @param {Object} req
 * @param {Object} res
 * @param {number} statusCode
 * @param {string} html
 * @param {Object<string,string>} [extraHeaders]
 */
function respondHtml(req, res, statusCode, html, extraHeaders) {
    cacheAndSend(req, res, statusCode,
        html, "text/html; charset=utf-8",
        extraHeaders,
        (r, body) => r.html(body));
}

/**
 * Mark an idempotency key as complete without caching a response.
 *
 * Useful when the handler sends a response via `res.json()` directly.
 * The key prevents concurrent duplicates but retries will re-execute
 * the handler (no cached body to replay).
 *
 * @param {Object} req
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
 * Delete expired idempotency keys.
 *
 * Call periodically (e.g. from `app.every(3600_000, idempotency.cleanup)`).
 *
 * @returns {number} Count of deleted rows.
 */
function cleanup() {
    const now = time.now();
    return db.exec("DELETE FROM _hull_idempotency_keys WHERE expires_at <= ?", [now]);
}

const idempotency = { init, middleware, respond, respondHtml, complete, cleanup };
export { idempotency };
