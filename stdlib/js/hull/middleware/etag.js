/*
 * hull:middleware:etag -- ETag response helpers
 *
 * Wraps res.json/text/html to add ETag support with 304 Not Modified.
 * Provides wrapper functions that replace res.json() in route handlers.
 *
 * Usage:
 *   import { etag } from "hull:middleware:etag";
 *   app.get("/api/items", (req, res) => {
 *       const data = db.query("SELECT * FROM items");
 *       etag.json(req, res, data);
 *   });
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { crypto } from "hull:crypto";

const MAX_BODY_SIZE = 1024 * 1024; // 1 MB

function compute(body) {
    if (!body || body.length === 0) return null;
    const hash = crypto.sha256(body);
    return 'W/"' + hash.substring(0, 16) + '"';
}

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
