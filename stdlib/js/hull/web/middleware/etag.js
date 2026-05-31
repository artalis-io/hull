/**
 * @file hull:web:middleware:etag
 * @module hull:web:middleware:etag
 * @description ETag response helpers — `etag.json`, `etag.text`, `etag.html`.
 *   Lua parity: `hull.web.middleware.etag`.
 *
 * Not a Keel middleware (no `(req, res) -> number` signature). Instead,
 * handlers call `etag.json(req, res, data)` in place of `res.json(data)`
 * and the helper:
 *
 *   1. Encodes the body.
 *   2. Computes a weak ETag `W/"<first 16 hex of SHA-256>"`.
 *   3. Returns `304 Not Modified` when `If-None-Match` matches.
 *   4. Otherwise sends the full response with an `ETag` header.
 *
 * Skips ETag work for non-GET/HEAD methods and bodies larger than 1 MB.
 *
 * @license AGPL-3.0-or-later
 *
 * @example
 * app.get("/api/items", (req, res) => {
 *     etag.json(req, res, db.query("SELECT * FROM items"));
 * });
 */

import { crypto } from "hull:crypto";

const MAX_BODY_SIZE = 1024 * 1024; // 1 MB

/**
 * Compute a weak ETag from a body string.
 *
 * @param {string} body
 * @returns {?string}  `W/"<16-hex>"`, or `null` for empty/error.
 */
function compute(body) {
    if (!body || body.length === 0) return null;
    const hash = crypto.sha256(body);
    return 'W/"' + hash.substring(0, 16) + '"';
}

/**
 * Does the request's `If-None-Match` match `tag`?
 *
 * Accepts a comma-separated list and the `*` wildcard.
 *
 * @param {Object}  req
 * @param {?string} tag
 * @returns {boolean}
 */
function matches(req, tag) {
    if (!tag) return false;
    const inm = req.header("If-None-Match");
    if (!inm) return false;
    if (inm.trim() === "*") return true;
    const parts = inm.split(",");
    for (let i = 0; i < parts.length; i++) {
        if (parts[i].trim() === tag) return true;
    }
    return false;
}

/**
 * Send a JSON response with ETag support.
 *
 * Returns `304 Not Modified` when `If-None-Match` matches; otherwise
 * sends the encoded body with an `ETag` header.
 *
 * @param {Object} req
 * @param {Object} res
 * @param {*}      data    Body data (JSON-encoded).
 * @param {number} [status=200]
 */
function jsonEtag(req, res, data, status) {
    if (req.method !== "GET" && req.method !== "HEAD") {
        if (status) res.status(status);
        res.json(data);
        return;
    }

    const body = JSON.stringify(data);
    if (body.length > MAX_BODY_SIZE) {
        if (status) res.status(status);
        res.json(data);
        return;
    }

    const tag = compute(body);
    if (matches(req, tag)) {
        res.status(304).header("ETag", tag);
        return;
    }

    res.header("ETag", tag);
    if (status) res.status(status);
    res.json(data);
}

/**
 * Send a text response with ETag support.
 * @param {Object} req
 * @param {Object} res
 * @param {string} text
 * @param {number} [status=200]
 */
function textEtag(req, res, text, status) {
    if (req.method !== "GET" && req.method !== "HEAD") {
        if (status) res.status(status);
        res.text(text);
        return;
    }

    if (text.length > MAX_BODY_SIZE) {
        if (status) res.status(status);
        res.text(text);
        return;
    }

    const tag = compute(text);
    if (matches(req, tag)) {
        res.status(304).header("ETag", tag);
        return;
    }

    res.header("ETag", tag);
    if (status) res.status(status);
    res.text(text);
}

/**
 * Send an HTML response with ETag support.
 * @param {Object} req
 * @param {Object} res
 * @param {string} html
 * @param {number} [status=200]
 */
function htmlEtag(req, res, html, status) {
    if (req.method !== "GET" && req.method !== "HEAD") {
        if (status) res.status(status);
        res.html(html);
        return;
    }

    if (html.length > MAX_BODY_SIZE) {
        if (status) res.status(status);
        res.html(html);
        return;
    }

    const tag = compute(html);
    if (matches(req, tag)) {
        res.status(304).header("ETag", tag);
        return;
    }

    res.header("ETag", tag);
    if (status) res.status(status);
    res.html(html);
}

const etag = { compute, matches, json: jsonEtag, text: textEtag, html: htmlEtag };
export { etag };
