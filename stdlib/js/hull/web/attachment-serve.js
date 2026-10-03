/**
 * @file hull:web:attachment-serve
 * @module hull:web:attachment-serve
 * @description Auth-gated HTTP response helper for hull:attachment.
 *   JS parity for `hull.web.attachment-serve` (Lua).
 *
 * Thin web layer over hull:attachment - takes `(req, res, id, opts)`
 * and produces an auth-gated, ETagged, Content-Disposition-bearing
 * response body. The core attachment module is intentionally HTTP-free
 * (works in CLI tools too); this is the only piece coupled to
 * hull:http-server and `res.bytes`.
 *
 * Design notes:
 *
 *   - **Default deny.** If `opts.authCheck` is not supplied, the
 *     helper responds 403. Hull's other auth modules (session,
 *     rbac) follow the same fail-closed pattern.
 *   - **Strong ETag from blob_id.** Because the underlying blob
 *     store is content-addressed by SHA-256, the blob_id IS a
 *     genuine cryptographic fingerprint of the bytes - strong
 *     ETags are correct here (no risk of two attachments with the
 *     same id but different bytes). We use `"<full-64-hex>"`.
 *   - **Content-Disposition (RFC 5987).** Browsers should save
 *     uploads under the original filename even when it contains
 *     non-ASCII. The wire format is:
 *
 *         attachment; filename="<ascii-fallback>"; filename*=UTF-8''<pct-encoded>
 *
 *     Old clients honour `filename=`; modern ones prefer `filename*=`.
 *
 * @license AGPL-3.0-or-later
 */

import { attachment } from "hull:attachment";
import { blob } from "hull:blob";
import { encoding } from "hull:encoding";

// RFC 5987 attr-char set: ALPHA / DIGIT / !#$&+-.^_`|~ - the RFC 3986
// unreserved set plus these. Anything else is percent-encoded.
const ATTR_CHAR = { keep: "!#$&+^`|" };

// A stored name is not ours to refuse, so a lone surrogate (which has no
// UTF-8 form) becomes U+FFFD instead of failing the response.
function wellFormed(name) {
    return name.replace(/[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/g,
                        "\ufffd");
}

// ASCII fallback: iterate the UTF-8 byte stream (NOT JS chars) so
// the result matches the Lua sibling byte-for-byte. Non-ASCII bytes
// → `_`; `"` and `\` get backslash-escaped so the quoted-string
// can't break out of the header field.
function asciiFallbackBytes(bytes) {
    let out = "";
    for (let i = 0; i < bytes.length; i++) {
        const b = bytes.charCodeAt(i);
        if (b === 0x22 || b === 0x5C) {        // " or \
            out += "\\" + String.fromCharCode(b);
        } else if (b < 0x20 || b > 0x7E) {     // non-printable / non-ASCII
            out += "_";
        } else {
            out += String.fromCharCode(b);
        }
    }
    return out;
}

// Build full Content-Disposition with both ASCII fallback and the
// RFC 5987 percent-encoded UTF-8 form. Both halves operate on the
// SAME UTF-8 byte stream so the output matches the Lua sibling
// byte-for-byte for any input (BMP, supplementary plane, surrogates).
function contentDisposition(name) {
    const text = wellFormed(name);
    const bytes = encoding.utf8.encode(text);
    const pct = encoding.url.encode(text, ATTR_CHAR);
    return 'attachment; filename="' + asciiFallbackBytes(bytes) +
           '"; filename*=UTF-8\'\'' + pct;
}

/**
 * Serve an attachment over HTTP.
 *
 * @param {Object} req
 * @param {Object} res
 * @param {string} id
 * @param {Object} [opts]
 * @param {function(Object, Object): boolean} [opts.authCheck]
 *   REQUIRED for non-403 responses. Receives the live metadata
 *   row so the check can do per-tenant / per-user gating. Omit
 *   to deny unconditionally.
 */
function serve(req, res, id, opts) {
    const o = opts || {};

    const meta = attachment.metadata(id);
    if (!meta) {
        res.status(404);
        res.json({ error: "not found" });
        return;
    }

    // Default-deny: caller must explicitly supply authCheck AND it
    // must return truthy. Missing function, false, or undefined → 403.
    if (typeof o.authCheck !== "function" || !o.authCheck(req, meta)) {
        res.status(403);
        res.json({ error: "forbidden" });
        return;
    }

    // Strong ETag from full blob_id SHA - content-addressed dedup
    // means this is a genuine cryptographic fingerprint of the bytes.
    const etag = '"' + meta.blob_id + '"';

    // If-None-Match → 304. Accepts comma-separated values + `*` wildcard.
    const inm = req.headers && req.headers["if-none-match"];
    if (inm) {
        if (/^\s*\*\s*$/.test(inm)) {
            res.header("ETag", etag);
            res.status(304);
            return;
        }
        for (const part of inm.split(",")) {
            if (part.trim() === etag) {
                // RFC 9110 15.4.5: a 304 carries the ETag the 200 would have.
                res.header("ETag", etag);
                res.status(304);
                return;
            }
        }
    }

    const bytes = blob.get(meta.blob_id);
    if (!bytes) {
        // Metadata says it exists but the blob is missing. Treat as
        // 410 Gone so caches know to drop their copy.
        res.status(410);
        res.json({ error: "blob missing" });
        return;
    }

    res.header("Content-Type", meta.mime);
    res.header("Content-Disposition", contentDisposition(meta.original_name));
    res.header("ETag", etag);
    // The stored MIME type is whatever the uploader claimed. nosniff stops a
    // browser second-guessing it - rendering an uploaded "text/plain" that
    // looks like HTML as HTML, on this origin.
    res.header("X-Content-Type-Options", "nosniff");
    res.bytes(bytes);
}

export const attachmentServe = { serve };
