/**
 * @file hull:web:middleware:idempotency
 * @module hull:web:middleware:idempotency
 * @description Idempotency-Key middleware for safe POST retries. Lua parity:
 *   `hull.web.middleware.idempotency`.
 *
 * Prevents duplicate side effects when clients retry the same request by
 * caching the response keyed by `(principal_id, idempotency_key)`. A
 * `SHA-256(method || path || query || body)` fingerprint detects key reuse with a
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

import { internal as dbInternal } from "hull:db:_internal";
const db = dbInternal.connection();
import { crypto } from "hull:crypto";
import { encoding } from "hull:encoding";
import * as encNative from "hull:encoding:_native";
import { time } from "hull:time";
import { _test_hooks } from "hull:_test_hooks";
import { json } from "hull:json";
import { log } from "hull:log";
import { _request } from "hull:web:_request";
import { _text } from "hull:_text";

let idemTtl = 86400;
const HEADER_NAME = "idempotency-key";

// An in-flight key expires after this many seconds (or the TTL, if shorter).
// A handler that threw never completes its row, and with the full TTL every
// retry got 409 for a day - defeating the point of an idempotent retry. A
// completed response keeps the full TTL (completedExpiry).
const INFLIGHT_LEASE = 300;

function completedExpiry(req) {
    return time.now() + (req.ctx._idem_ttl !== undefined ? req.ctx._idem_ttl : idemTtl);
}

// A completion writes only the claim it belongs to: still in flight, with this
// request's fingerprint and claim time. A handler that ran past its lease had
// its row reclaimed by a retry (possibly with another body); completing later,
// it overwrote that row, so replays of the retry returned the first
// handler's response.
const CLAIM_GUARD = " AND state = 'inflight' AND fingerprint = ? AND created_at = ?";
// Matches the `key` column width (VARCHAR(255)); keys over this are rejected.
const MAX_KEY_LEN = 255;

// The `key` column, quoted for the connection's dialect (audit 39): KEY is a
// reserved word in MySQL, where every statement naming it bare - the CREATE
// TABLE included - was a syntax error. Quoting keeps the column name existing
// SQLite / Postgres tables have. Resolved per call: the internal connection
// is wired after module load.
function keyCol() {
    return db.quoteIdentifier("key");
}

// The stored response (audit 39; see the Lua sibling). It used to be written
// AFTER it was sent, as raw text: a body Postgres refuses (a NUL, a lone
// surrogate) made that UPDATE throw after the handler's side effect had
// committed - a 500, the row 'inflight' for its lease, and a retry after the
// lease ran the handler AGAIN. Now the body and the headers blob are stored
// base64 encoded BEFORE the response is sent, and a write that still fails
// marks the row STATE_UNSTORED, whose retries get a fixed 409.
//
// A v2 row has response_headers = STORED_V2 + base64(JSON of the headers)
// and response_body = base64(body bytes); a JS string is stored as its
// WTF-8 bytes (UTF-8, with a lone surrogate as its own 3-byte sequence, as
// the runtime sends it), so the replay is byte-exact. A row an earlier Hull
// wrote (a bare JSON object, or NULL) replays as before.
const STORED_V2 = "v2:";
const STATE_UNSTORED = "unstored";
// Test seam (hull:_test_hooks failStore): makes the store throw.
let failStoreForTest = false;

// The largest response body (in WTF-8 bytes) stored for replay (audit 40).
// A larger one marks the row unstored before anything is encoded, so the
// store can never be what trips the run's limits after the handler's side
// effects committed. Lua parity: MAX_STORED_BYTES.
const MAX_STORED_BYTES = 32 * 1024 * 1024;
// The stored headers blob: the replay decodes it in JS, and a response
// cannot send more than 64 KiB of headers anyway.
const MAX_STORED_HEADER_BYTES = 256 * 1024;

// base64 of a JS string's WTF-8 bytes (UTF-8, a lone surrogate as its own
// 3-byte sequence, as the runtime sends it), in one native call (audit 40:
// a pure-JS per-code-unit loop here tripped the instruction limit on a 12 MB
// body). Throws when the bytes are past `max`.
function storedText(str, max) {
    if (str.length > max) throw new RangeError("response too large to store");
    const enc = encNative.textBase64Encode(str, max);
    if (typeof enc !== "string") throw new RangeError("response too large to store");
    return enc;
}

/* M-7 (Phase 5) + Phase 6 audit M-1: allowlist of headers safe to replay
 * or cache from a previous response. Excludes credential-bearing /
 * session-binding headers; explicitly allowlists common security
 * response headers (HSTS/CSP/etc.) so they aren't silently dropped on
 * replay. NO blanket X-* - the previous version was too permissive
 * (would replay X-Auth-Token, X-API-Key, X-CSRF-Token, etc.). */
const REPLAYABLE_HEADERS = {
    __proto__: null,   // "constructor" etc. are not replayable (audit 20)
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
    // allowlist addition can't override the deny - defense in depth
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

// A ttl is a positive, finite NUMBER of seconds (audit 15): a string one
// ("86400") concatenated onto the expiry (now + "86400"). Same rule as the
// Lua twin and auth.login; anything else throws.
//
// It is also a whole number of seconds no larger than MAX_TTL (366 days;
// audit 26): an unbounded one made `now + ttl` overflow a 32-bit INTEGER
// expiry column on PostgreSQL / MySQL (and run past 2038).
const MAX_TTL = 31622400;
function checkTtl(who, v) {
    if (!(typeof v === "number" && Number.isInteger(v) && v > 0 && v <= MAX_TTL))
        throw new TypeError(who + " must be a whole number of seconds in [1, "
            + MAX_TTL + "]");
    return v;
}

/**
 * Initialize the `_hull_idempotency_keys` SQLite table. Idempotent.
 *
 * @param {Object} [opts]
 * @param {number} [opts.ttl=86400]  Key lifetime in seconds.
 */
function init(opts) {
    const o = opts || {};
    if (o.ttl !== undefined) idemTtl = checkTtl("idempotency.init: ttl", o.ttl);

    // MySQL's TEXT holds 64 KiB; a stored body (base64, 4/3 of the bytes)
    // needs more there. MEDIUMTEXT holds 16 MiB; a body past even that is
    // marked unstored rather than truncated (strict mode refuses it).
    // `state` is a VARCHAR: MySQL gives no TEXT column a DEFAULT.
    const bodyType = db.backendName === "mysql" ? "MEDIUMTEXT" : "TEXT";
    const k = keyCol();
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_idempotency_keys (" +
        "  " + k + " VARCHAR(255) NOT NULL," +
        "  principal_id   VARCHAR(255) NOT NULL DEFAULT '__anon'," +
        "  fingerprint    TEXT NOT NULL," +
        "  endpoint       TEXT NOT NULL," +
        "  status         INTEGER," +
        "  response_body  " + bodyType + "," +
        "  response_headers " + bodyType + "," +
        "  state          VARCHAR(16) NOT NULL DEFAULT 'inflight'," +
        "  created_at     INTEGER NOT NULL," +
        "  expires_at     INTEGER NOT NULL," +
        "  PRIMARY KEY (principal_id, " + k + ")" +
        ")"
    );
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_idem_expires " +
        "ON _hull_idempotency_keys(expires_at)"
    );
}

/**
 * The query string as fingerprint input: its pairs sorted by name, each
 * length-prefixed so that no two different queries encode alike.
 */
function queryPart(q) {
    if (!q || typeof q !== "object") return "";
    const names = Object.keys(q).sort();
    const parts = [];
    for (const k of names) {
        const v = String(q[k]);
        parts.push(k.length + ":" + k + v.length + ":" + v);
    }
    return parts.join("");
}

/**
 * Compute a request fingerprint: SHA-256(method + path + query + body).
 * The query is part of it: without it, reusing a key for
 * `POST /transfer?to=alice` and then `?to=bob` (same body) replayed
 * alice's response instead of answering 409, and bob's transfer was never
 * made.
 */
function computeFingerprint(req) {
    const head = (req.method || "") + "\0" + (req.path || "") + "\0"
        + queryPart(req.query) + "\0";
    // A body that is not valid UTF-8 reads with every invalid sequence as
    // U+FFFD, so two binary bodies could fingerprint the same and a key
    // reused across them replayed the first response (audit 24): hash its
    // bytes. The leading \x01 (never in a method) keeps the two forms apart.
    if (req.bodyBytes instanceof ArrayBuffer)
        return encoding.hex.encode(crypto.sha256("\x01" + head
            + encoding.hex.encode(crypto.sha256(req.bodyBytes))));
    return encoding.hex.encode(crypto.sha256(head + (req.body || "")));
}

/**
 * Build a post-body idempotency middleware.
 *
 * Reads the key from `req.header(headerName)`. Same key+body → cached
 * replay; different body → 409; missing header → handler runs.
 *
 * @param {Object} [opts]
 * @param {(req) => string} [opts.getPrincipal]
 *   Returns a stable per-user key for scoping. Default: `"session:"` + the
 *   session's `user_id`, else `"user:"` + the JWT user's `sub` / `id` /
 *   `user_id` (`req.ctx.user`, set by auth.jwtMiddleware), else `"__anon"` -
 *   which every anonymous caller shares, so mount it after authentication (a
 *   warning is logged once when 20 keyed requests in a row were anonymous).
 *   A CUSTOM getPrincipal that returns null / undefined / false / "" does
 *   not share "__anon" (audit 11): the request skips the idempotency layer
 *   (the handler runs, nothing is stored or replayed) and a warning is
 *   logged once per middleware. Return "__anon" explicitly to share a scope.
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
// The default principal: "session:<user_id>" for a session user, else
// "user:<id>" for a JWT user, else "__anon". The two kinds of user are
// prefixed apart (audit 9; see the Lua sibling).
//
// Both ids go through hull:web:_request's userId, as the Lua twin's do
// (audit 23): "" is no id, an object is no id, 0 is a user. The JWT id is the
// first of sub / id / user_id that is one.
function defaultPrincipal(req) {
    const ctx = req.ctx;
    const sess = ctx && typeof ctx === "object" ? ctx.session : undefined;
    if (sess && typeof sess === "object") {
        const sid = _request.userId(sess.user_id);
        if (sid !== null) return "session:" + sid;
    }
    // A JWT-authenticated user (auth.jwtMiddleware). Without this every
    // bearer-token user shared "__anon", so one could replay another's
    // stored response, or squat their key, by knowing key and body.
    const u = ctx && typeof ctx === "object" ? ctx.user : undefined;
    if (u && typeof u === "object") {
        for (const k of ["sub", "id", "user_id"]) {
            const id = _request.userId(u[k]);
            if (id !== null) return "user:" + id;
        }
    }
    return "__anon";
}

// The principal as the principal_id column (VARCHAR(255)) stores it, for the
// default and a custom getPrincipal alike (audit 10; see the Lua sibling):
// null / undefined / false / "" is "__anon", a number its String(), anything
// over the column width (or already spelled like a hashed one) "sha256:<hex>"
// of itself, and any other type (an object, a Promise) an error.
function normPrincipal(p) {
    if (p === undefined || p === null || p === false || p === "") return "__anon";
    if (typeof p === "number" || typeof p === "bigint") {
        p = String(p);
    } else if (typeof p !== "string") {
        throw new TypeError("idempotency: getPrincipal must return a string or a number, got "
            + typeof p);
    }
    // Text the column cannot store as is (a NUL, a lone surrogate; audit 39)
    // is hashed too: Postgres / MySQL refused the claim's INSERT.
    if (p.length > MAX_KEY_LEN || p.startsWith("sha256:") || p.indexOf("\0") >= 0
        || _text.storable(p, 4 * MAX_KEY_LEN) !== p)
        return "sha256:" + encoding.hex.encode(crypto.sha256(p));
    return p;
}

// Keyed requests in a row that may all be "__anon" before the default
// principal warns, once per middleware (see the Lua sibling).
const ANON_WARN_AFTER = 20;

function middleware(opts) {
    const o = opts || {};

    let anonRun = 0, anonWarned = false;
    const getPrincipal = o.getPrincipal || function(req) {
        const p = defaultPrincipal(req);
        if (p !== "__anon") {
            anonRun = 0;
        } else if (!anonWarned && ++anonRun >= ANON_WARN_AFTER) {
            anonWarned = true;
            log.warn("idempotency: the last " + ANON_WARN_AFTER + " keyed requests "
                + "had no session or JWT user, so they all share the '__anon' "
                + "principal (one caller can replay another's stored response). "
                + "Mount the middleware after authentication, or pass getPrincipal.");
        }
        return p;
    };

    const ttl = o.ttl !== undefined ? checkTtl("idempotency.middleware: ttl", o.ttl) : idemTtl;
    const headerName = o.headerName || HEADER_NAME;

    const methodList = o.methods || ["POST"];
    const methods = {};
    for (let i = 0; i < methodList.length; i++)
        methods[methodList[i]] = true;

    // Counter for probabilistic cleanup (every 100 requests)
    let requestCount = 0;
    const CLEANUP_INTERVAL = 100;

    const custom = !!o.getPrincipal;
    let customWarned = false;

    return function(req, res) {
        if (!methods[req.method])
            return 0;

        const key = req.header(headerName);
        if (!key || key === "")
            return 0;
        // The key is the PK's client-supplied half (VARCHAR(255)). Reject an
        // over-length key rather than let MySQL silently truncate it, which
        // would collide two distinct keys sharing a 255-char prefix and replay
        // the wrong cached response. Uniform across every backend.
        if (key.length > MAX_KEY_LEN) {
            res.status(400);
            res.json({ error: "idempotency-key too long (max " + MAX_KEY_LEN + ")" });
            return 1;
        }
        // Printable ASCII only (audit 39; see the Lua sibling): a key the
        // column refuses failed the claim's INSERT - a 500 for a client error.
        if (typeof key !== "string" || /[^\x20-\x7e]/.test(key)) {
            res.status(400);
            res.json({ error: "idempotency-key must be printable ASCII" });
            return 1;
        }

        if (!req.ctx) req.ctx = {};

        const rawPrincipal = getPrincipal(req);
        // A custom getPrincipal with no answer (audit 11; see the Lua
        // sibling): the request runs without idempotency, never "__anon".
        if (custom && (rawPrincipal === undefined || rawPrincipal === null
                       || rawPrincipal === false || rawPrincipal === "")) {
            if (!customWarned) {
                customWarned = true;
                log.warn("idempotency: getPrincipal returned " + String(rawPrincipal)
                    + "; the request runs without idempotency "
                    + "(return \"__anon\" to share one scope)");
            }
            return 0;
        }
        const principalId = normPrincipal(rawPrincipal);
        const fingerprint = computeFingerprint(req);
        // The path is request text: stored only for inspection, as text
        // every backend accepts (audit 39).
        const endpoint = _text.storable(String(req.method) + " " + String(req.path), 1024, true);
        const now = time.now();
        const K = keyCol();

        // Periodic cleanup
        requestCount++;
        if (requestCount >= CLEANUP_INTERVAL) {
            requestCount = 0;
            db.exec("DELETE FROM _hull_idempotency_keys WHERE expires_at <= ?", [now]);
        }

        // Check for existing key
        const rows = db.query(
            "SELECT fingerprint, state, status, response_body, response_headers, expires_at " +
            "FROM _hull_idempotency_keys WHERE principal_id = ? AND " + K + " = ?",
            [principalId, key]
        );

        if (rows && rows.length > 0) {
            const row = rows[0];

            if (row.expires_at <= now) {
                // Expired: delete and treat as new. Only while it is still
                // expired: a concurrent request may have replaced it with a
                // fresh claim since the SELECT, and an unguarded DELETE
                // removed that.
                db.exec(
                    "DELETE FROM _hull_idempotency_keys WHERE principal_id = ? AND " + K + " = ? " +
                    "AND expires_at <= ?",
                    [principalId, key, now]
                );
            } else {
                // Fingerprint mismatch. Constant-time comparison even though
                // fingerprints are SHA-256 of public inputs (method+path+body),
                // for consistency with jwt.js / csrf.js.
                // Hoist `rfp` so the `charCodeAt` call below can't read off a
                // (theoretically) null row.fingerprint - schema has NOT NULL
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

                // Ran, but its response could not be stored (audit 39): the
                // side effect happened, so the handler must not run again,
                // and there is nothing to replay.
                if (row.state === STATE_UNSTORED) {
                    res.status(409);
                    res.json({ error: "request with this idempotency key was already "
                        + "processed; its response could not be stored for replay" });
                    return 1;
                }

                // Completed with cached response
                if (row.state === "complete" && row.status) {
                    // A v2 row holds both blobs base64 encoded (bytes, sent
                    // as is); an older row the raw text. An undecodable v2
                    // row (tampered) replays an empty body.
                    let headersJson = row.response_headers;
                    let bodyText = row.response_body;
                    let bodyBytes = null;
                    if (typeof headersJson === "string" && headersJson.startsWith(STORED_V2)) {
                        const hb = encoding.base64.decode(headersJson.slice(STORED_V2.length));
                        headersJson = hb === null ? null : encoding.utf8.decode(hb);
                        // Decoded natively straight to an ArrayBuffer for
                        // res.bytes (audit 40: a per-byte JS loop here).
                        const bb = typeof bodyText === "string"
                            ? encNative.base64DecodeBuffer(bodyText) : null;
                        bodyText = null;
                        bodyBytes = bb === null ? new ArrayBuffer(0) : bb;
                    }
                    res.status(row.status);
                    // Decode once. Walk once. M-7 filtering AND
                    // Content-Type detection both happen in the
                    // single pass so a corrupted blob can't throw
                    // an uncaught SyntaxError from a second decode
                    // path (audit J#1).
                    let replayedCt = null;
                    if (typeof headersJson === "string" && headersJson) {
                        let headers;
                        try { headers = json.decode(headersJson); }
                        catch (_e) { headers = null; }
                        if (headers && typeof headers === "object") {
                            const keys = Object.keys(headers);
                            for (let i = 0; i < keys.length; i++) {
                                const k = keys[i];
                                const v = headers[k];
                                // M-7: defense-in-depth - drop credential-
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
                    if (bodyBytes !== null) {
                        if (!replayedCt) res.header("Content-Type", "application/json");
                        res.bytes(bodyBytes);
                    } else if (bodyText) {
                        // Default to application/json for back-compat
                        // with old cached rows from before respondHtml()
                        // existed; respond()/respondHtml() always stash
                        // a Content-Type now so this branch is rare.
                        if (!replayedCt) {
                            res.header("Content-Type", "application/json");
                        }
                        res.text(bodyText);
                    } else {
                        res.text("");
                    }
                    return 1;
                }

                // Complete but no cached response: delete and re-run
                db.exec(
                    "DELETE FROM _hull_idempotency_keys WHERE principal_id = ? AND " + K + " = ? " +
                    "AND state = 'complete' AND status IS NULL",
                    [principalId, key]
                );
            }
        }

        // Insert in-flight record (insert-if-absent guards the race with
        // a concurrent request claiming the same key).
        const inserted = db.insertIfAbsent(
            "_hull_idempotency_keys",
            ["principal_id", K],
            [K, "principal_id", "fingerprint", "endpoint",
             "state", "created_at", "expires_at"],
            [key, principalId, fingerprint, endpoint,
             "inflight", now, now + Math.min(ttl, INFLIGHT_LEASE)]
        );
        if (inserted === 0) {
            res.status(409);
            res.json({ error: "request with this idempotency key is already in progress" });
            return 1;
        }

        // Store key info in context for respond() to use
        req.ctx._idem_key = key;
        req.ctx._idem_principal = principalId;
        req.ctx._idem_ttl = ttl;
        // This request's claim, for the completing UPDATE (see CLAIM_GUARD).
        req.ctx._idem_fingerprint = fingerprint;
        req.ctx._idem_created = now;

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
        // Stored BEFORE the response is sent, base64 encoded (audit 39; see
        // STORED_V2). A write that still fails (a body past the column, a
        // lost connection) marks the row unstored - a retry gets 409, never
        // a second run of the handler - and the response is sent anyway.
        const where = " WHERE principal_id = ? AND " + keyCol() + " = ?" + CLAIM_GUARD;
        const claim = [req.ctx._idem_principal, req.ctx._idem_key,
                       req.ctx._idem_fingerprint, req.ctx._idem_created];
        try {
            if (failStoreForTest) throw new Error("injected store failure");
            // Size-checked first, then encoded natively (audit 40).
            const bodyEnc = storedText(typeof bodyStr === "string" ? bodyStr : String(bodyStr ?? ""),
                                       MAX_STORED_BYTES);
            const headersStr = STORED_V2 + storedText(json.encode(filtered), MAX_STORED_HEADER_BYTES);
            db.exec(
                "UPDATE _hull_idempotency_keys SET state = 'complete', status = ?, " +
                "response_body = ?, response_headers = ?, expires_at = ?" + where,
                [statusCode, bodyEnc, headersStr, completedExpiry(req)].concat(claim)
            );
        } catch (e) {
            log.warn("idempotency: the response could not be stored for replay ("
                + _text.storable(e && e.message !== undefined ? e.message : e, 512, true)
                + "); retries of this key get 409");
            try {
                db.exec(
                    "UPDATE _hull_idempotency_keys SET state = '" + STATE_UNSTORED + "', " +
                    "status = NULL, response_body = NULL, response_headers = NULL, " +
                    "expires_at = ?" + where,
                    [completedExpiry(req)].concat(claim)
                );
            } catch (e2) {
                log.error("idempotency: the key could not be marked processed ("
                    + _text.storable(e2 && e2.message !== undefined ? e2.message : e2, 512, true)
                    + "); a retry after the " + INFLIGHT_LEASE + " s lease runs the handler again");
            }
        }
    }

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
            "UPDATE _hull_idempotency_keys SET state = 'complete', expires_at = ? " +
            "WHERE principal_id = ? AND " + keyCol() + " = ?" + CLAIM_GUARD,
            [completedExpiry(req), req.ctx._idem_principal, req.ctx._idem_key,
             req.ctx._idem_fingerprint, req.ctx._idem_created]
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

// Test-only hooks, registered with the stdlib-internal hull:_test_hooks
// rather than exported (audit 16): the suite reads a key's row back from
// inside the module, where the _hull_* namespace check admits the query.
_test_hooks.register("hull:web:middleware:idempotency", {
    rows(key) {
        return db.query("SELECT state, status, response_body, response_headers " +
                        "FROM _hull_idempotency_keys WHERE " + keyCol() + " = ?", [key]);
    },
    // The principal helpers (audit 23: they were exported on the module).
    defaultPrincipal,
    normPrincipal,
    computeFingerprint,
    // Make the response store fail (audit 39: a failed store must leave the
    // key processed, never re-runnable).
    failStore(on) { failStoreForTest = !!on; },
    // A completed row in the format an earlier Hull wrote (raw body, bare
    // JSON headers), for the backward-compatibility check.
    putLegacy(key, principal, fingerprint, status, body, headersJson) {
        const now = time.now();
        db.exec("INSERT INTO _hull_idempotency_keys (" + keyCol() + ", principal_id, " +
            "fingerprint, endpoint, status, response_body, response_headers, state, " +
            "created_at, expires_at) VALUES (?, ?, ?, 'POST /legacy', ?, ?, ?, " +
            "'complete', ?, ?)",
            [key, principal, fingerprint, status, body, headersJson, now, now + 3600]);
    },
});
