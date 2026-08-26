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

// Manual UTF-8 encoder: JS strings are UTF-16, so iterating by
// charCodeAt yields code units, not UTF-8 bytes. RFC 5987 requires
// percent-encoding of UTF-8 octets, so we encode to bytes first.
// QuickJS doesn't bundle TextEncoder; this 20-liner covers the
// full BMP + surrogate pairs.
function utf8Bytes(str) {
    const out = [];
    for (let i = 0; i < str.length; i++) {
        let cp = str.charCodeAt(i);
        // High surrogate → combine with the next code unit (low
        // surrogate) into a single supplementary-plane code point.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < str.length) {
            const lo = str.charCodeAt(i + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) {
            out.push(cp);
        } else if (cp < 0x800) {
            out.push(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out.push(0xE0 | (cp >> 12),
                     0x80 | ((cp >> 6) & 0x3F),
                     0x80 | (cp & 0x3F));
        } else {
            out.push(0xF0 | (cp >> 18),
                     0x80 | ((cp >> 12) & 0x3F),
                     0x80 | ((cp >> 6) & 0x3F),
                     0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// RFC 5987 attr-char set: ALPHA / DIGIT / !#$&+-.^_`|~
// Anything else (including space, /, etc.) gets percent-encoded.
function pctEncodeByte(b) {
    if ((b >= 0x30 && b <= 0x39)   // 0-9
        || (b >= 0x41 && b <= 0x5A) // A-Z
        || (b >= 0x61 && b <= 0x7A) // a-z
        || b === 0x21 || b === 0x23 || b === 0x24 || b === 0x26  // ! # $ &
        || b === 0x2B || b === 0x2D || b === 0x2E || b === 0x5E  // + - . ^
        || b === 0x5F || b === 0x60 || b === 0x7C || b === 0x7E  // _ ` | ~
    ) {
        return String.fromCharCode(b);
    }
    return "%" + b.toString(16).toUpperCase().padStart(2, "0");
}

// ASCII fallback: iterate the UTF-8 byte stream (NOT JS chars) so
// the result matches the Lua sibling byte-for-byte. Non-ASCII bytes
// → `_`; `"` and `\` get backslash-escaped so the quoted-string
// can't break out of the header field.
function asciiFallbackBytes(bytes) {
    let out = "";
    for (const b of bytes) {
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
    const bytes = utf8Bytes(name);
    let pct = "";
    for (const b of bytes) pct += pctEncodeByte(b);
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
            res.status(304);
            return;
        }
        for (const part of inm.split(",")) {
            if (part.trim() === etag) {
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
    res.bytes(bytes);
}

export const attachmentServe = { serve };
