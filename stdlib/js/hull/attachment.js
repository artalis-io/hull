/**
 * @file hull:attachment
 * @module hull:attachment
 * @description Content-addressed attachment storage backed by
 *   hull/blob + SQLite metadata. JS parity for `hull.attachment` (Lua).
 *
 * Thin layer over hull:blob: each {@link store} streams a multipart
 * Part through `blob.writer()` (content-addressed, dedupes on disk
 * automatically) and inserts a metadata row in `_hull_attachments`
 * (id, blob_id, original_name, mime, declared_mime, size, uploaded_by,
 * uploaded_at, refcount).
 *
 * Lives flat at hull:attachment (not under hull:web:) — the core API
 * (store / read / metadata / delete) is FS + DB only and works in CLI
 * tools. The web-specific auth-gated `serve(req, res, id, { authCheck })`
 * helper lives in the separate hull:web:attachment-serve module.
 *
 * Portable SQL: `?` placeholders, INTEGER timestamps + refcount,
 * TEXT ids, no AUTOINCREMENT, no PRAGMAs.
 *
 * @license AGPL-3.0-or-later
 */

import { blob } from "hull:blob";
import { crypto } from "hull:crypto";
import { db } from "hull:db";
import { mime as mimeMod } from "hull:mime";
import { time } from "hull:time";

let maxSize = null;          // null = unlimited per upload
let mimeAllowlist = null;    // null = any MIME allowed

/**
 * Initialize attachment storage.
 *
 * Creates the `_hull_attachments` table if absent and stores module-
 * level limits. Call once at startup before any other attachment
 * function. The underlying hull:blob store must be initialized
 * separately via `blob.init({ dir: "..." })`.
 *
 * @param {Object} [opts]
 * @param {number} [opts.maxSize]  Per-attachment byte cap. Default: unlimited.
 * @param {string[]} [opts.mimeAllowlist]  Array of allowed sniffed MIMEs.
 *   Default: unlimited. Sniffed MIME is checked first; declared
 *   `Content-Type` is recorded separately for audit but NOT trusted
 *   for the allowlist.
 */
function init(opts) {
    const o = opts || {};
    if (o.maxSize !== undefined) maxSize = o.maxSize;
    if (o.mimeAllowlist !== undefined) {
        mimeAllowlist = new Set(o.mimeAllowlist);
    }

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_attachments (" +
        "  id TEXT PRIMARY KEY," +
        "  blob_id TEXT NOT NULL," +
        "  original_name TEXT NOT NULL," +
        "  mime TEXT NOT NULL," +
        "  declared_mime TEXT," +
        "  size INTEGER NOT NULL," +
        "  uploaded_by TEXT," +
        "  uploaded_at INTEGER NOT NULL," +
        "  refcount INTEGER NOT NULL DEFAULT 1" +
        ")"
    );
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx__hull_attachments_blob_id " +
        "ON _hull_attachments(blob_id)"
    );
}

// 32 hex chars from 16 random bytes — 128 bits of entropy.
function generateId() {
    const bytes = new Uint8Array(crypto.random(16));
    let id = "";
    for (let i = 0; i < bytes.length; i++)
        id += bytes[i].toString(16).padStart(2, "0");
    return id;
}

/**
 * Store a multipart part as an attachment.
 *
 * Streams the part through `blob.writer()` (content-addressed) and
 * inserts a metadata row with `refcount=1`. The blob layer dedupes
 * on disk automatically — two stores of the same bytes share one
 * on-disk blob but get distinct attachment ids.
 *
 * Sniffs the MIME type from the first chunk via `mime.sniff()` and
 * validates against the allowlist if `init` set one. The declared
 * `Content-Type` from the part is recorded separately (`declared_mime`)
 * but NOT trusted for allowlist enforcement — clients can spoof it.
 *
 * @param {Object} part  A multipart Part from `req.multipart()`. Must
 *   have `filename` (text fields throw).
 * @param {Object} [opts]
 * @param {string} [opts.uploadedBy]  Opaque caller identifier persisted
 *   to the metadata row for audit.
 * @returns {Promise<string>}  Fresh attachment id (32-char hex).
 * @throws  On size cap exceeded, MIME not in allowlist, or part is
 *   not a file upload.
 */
async function store(part, opts) {
    const o = opts || {};
    if (!part) throw new Error("attachment.store: part required");
    if (!part.filename) {
        throw new Error("attachment.store: part is not a file upload (no filename)");
    }

    const declared = part.contentType || "application/octet-stream";

    const w = blob.writer();
    let size = 0;
    let sniffed = null;

    try {
        for await (const chunk of part.chunks()) {
            const bytes = new Uint8Array(chunk);
            if (sniffed === null && bytes.length > 0) {
                sniffed = mimeMod.sniff(chunk) || "application/octet-stream";
                if (mimeAllowlist && !mimeAllowlist.has(sniffed)) {
                    w.abort();
                    throw new Error("attachment.store: MIME not allowed: " + sniffed);
                }
            }
            size += bytes.length;
            if (maxSize !== null && size > maxSize) {
                w.abort();
                throw new Error("attachment.store: PART_TOO_LARGE (size " +
                                size + " > max " + maxSize + ")");
            }
            w.write(chunk);
        }
    } catch (e) {
        // writer.abort() is idempotent on the JS side — safe to call again.
        try { w.abort(); } catch (_) { /* already aborted */ }
        throw e;
    }

    // Empty parts: nothing to sniff; record as octet-stream and skip
    // the allowlist check (no content to gate).
    if (sniffed === null) sniffed = "application/octet-stream";

    const { id: blobId, size: blobSize } = w.finalize();
    if (blobSize !== size) {
        throw new Error("attachment.store: blob writer size mismatch (" +
                        blobSize + " vs " + size + ")");
    }

    const id = generateId();
    db.exec(
        "INSERT INTO _hull_attachments " +
        "(id, blob_id, original_name, mime, declared_mime, " +
        " size, uploaded_by, uploaded_at, refcount) " +
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1)",
        [id, blobId, part.filename, sniffed, declared,
         size, o.uploadedBy || null, time.now()]
    );

    return id;
}

/**
 * Fetch metadata for an attachment.
 *
 * @param {string} id
 * @returns {Object|null}  Row with {id, blob_id, original_name, mime,
 *   declared_mime, size, uploaded_by, uploaded_at, refcount}, or
 *   null when no attachment exists with that id.
 */
function metadata(id) {
    const rows = db.query(
        "SELECT id, blob_id, original_name, mime, declared_mime, " +
        "       size, uploaded_by, uploaded_at, refcount " +
        "FROM _hull_attachments WHERE id = ?",
        [id]
    );
    if (!rows || rows.length === 0) return null;
    return rows[0];
}

/**
 * Read an attachment's bytes.
 *
 * Materialises the whole blob in memory. For large files prefer
 * {@link readToFile} or open a reader via `blob.reader(blobId)` and
 * consume chunks.
 *
 * @param {string} id
 * @returns {ArrayBuffer|null}  Raw bytes, or null when missing.
 */
function read(id) {
    const meta = metadata(id);
    if (!meta) return null;
    return blob.get(meta.blob_id);
}

// attachment.readToFile, attachment.delete, and attachment.serve land
// in PR 2. The first brings hull:fs as a (call-site-optional) dep
// that's cleanest to wire alongside the web serve helper; the second
// + third are the GC + auth-gated response slice.

export const attachment = { init, store, metadata, read };
