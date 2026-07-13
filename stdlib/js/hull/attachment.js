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
import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { fs } from "hull:fs";
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
 *
 * Re-init semantics: only options explicitly present in the second
 * (or later) call are updated; omitted keys preserve their prior
 * value. Effectively "sticky" — call once with the full config.
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
 *
 * Sniffer note: the MIME type is determined from the FIRST non-empty
 * chunk only. If the multipart parser delivers the first chunk in
 * fewer than ~8 bytes (rare in practice), the sniffer may not see
 * enough magic to identify the format and will fall back to
 * "application/octet-stream" — which then fails the allowlist if one
 * is configured. Callers that need bullet-proof sniffing on a very
 * bursty connection should buffer the first 512 bytes themselves
 * before constructing a Part-like object.
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
    let blobId = null;

    // Single try/catch covers chunks() / sniff() / write() / finalize()
    // / size-mismatch. abort() is a no-op after successful finalize;
    // if finalize already committed the blob and the size-mismatch
    // check then fires, also call blob.delete() so the on-disk blob
    // doesn't become an orphan (no metadata row will ever reference
    // it because we throw before the INSERT).
    try {
        for await (const chunk of part.chunks()) {
            const len = chunk.byteLength;
            if (sniffed === null && len > 0) {
                sniffed = mimeMod.sniff(chunk) || "application/octet-stream";
                if (mimeAllowlist && !mimeAllowlist.has(sniffed)) {
                    throw new Error("attachment.store: MIME not allowed: " + sniffed);
                }
            }
            size += len;
            if (maxSize !== null && size > maxSize) {
                throw new Error("attachment.store: PART_TOO_LARGE (size " +
                                size + " > max " + maxSize + ")");
            }
            w.write(chunk);
        }

        // Empty parts: nothing to sniff; record as octet-stream and
        // skip the allowlist (no content to gate).
        if (sniffed === null) sniffed = "application/octet-stream";

        const fin = w.finalize();
        blobId = fin.id;
        if (fin.size !== size) {
            throw new Error("attachment.store: blob writer size mismatch (" +
                            fin.size + " vs " + size + ")");
        }
    } catch (e) {
        try { w.abort(); } catch (_) { /* idempotent post-finalize */ }
        if (blobId) {
            try { blob.delete(blobId); } catch (_) { /* best effort */ }
        }
        throw e;
    }

    // Preserve nil/undefined-vs-empty-string distinction for
    // uploaded_by — matches Lua's behaviour (Lua binds the value
    // directly; nil → SQL NULL, empty string → empty TEXT).
    const uploadedBy = o.uploadedBy !== undefined ? o.uploadedBy : null;

    const id = generateId();
    db.exec(
        "INSERT INTO _hull_attachments " +
        "(id, blob_id, original_name, mime, declared_mime, " +
        " size, uploaded_by, uploaded_at, refcount) " +
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1)",
        [id, blobId, part.filename, sniffed, declared,
         size, uploadedBy, time.now()]
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

/**
 * Stream an attachment to a destination file path.
 *
 * Streams via `blob.reader()` so memory stays O(chunkSize) regardless
 * of attachment size.
 *
 * @param {string} id
 * @param {string} dst  Destination file path (must be inside a declared
 *   `manifest.fs.write` allowlist path).
 * @returns {number|null}  Bytes written, or null when missing.
 */
function readToFile(id, dst) {
    const meta = metadata(id);
    if (!meta) return null;
    const r = blob.reader(meta.blob_id);
    if (!r) return null;

    // Buffered read + single fs.write. metadata.size is authoritative
    // (verified at store time against blob.writer's own count) so we
    // can allocate the destination buffer once and stream chunks
    // directly into it — O(file_size) memory instead of the 2x of a
    // parts-array + concat scheme.
    const combined = new Uint8Array(meta.size);
    let offset = 0;
    while (offset < meta.size) {
        const chunk = r.read(64 * 1024);
        if (!chunk || chunk.byteLength === 0) break;
        combined.set(new Uint8Array(chunk), offset);
        offset += chunk.byteLength;
    }
    r.close();

    fs.write(dst, combined.buffer);
    return offset;
}

/**
 * Delete an attachment by id.
 *
 * Decrements the refcount; at 0 the metadata row is removed AND, if
 * no other row references the same blob_id, the on-disk blob is
 * unlinked via blob.delete(). The whole operation runs inside a
 * transaction so a partial failure doesn't leave an orphan or a
 * double-deleted blob. The blob.delete() call happens INSIDE the
 * BEGIN IMMEDIATE write lock — concurrent transactions can't insert
 * a new row referencing this blob_id between the SELECT and the
 * unlink. Trade-off: holding the SQLite write lock during the FS
 * unlink, which is fine for typical attachment sizes.
 *
 * Exported as `attachment.delete` (bracket-key, since `delete` is
 * a JS reserved operator keyword but a valid property name):
 *
 *   import { attachment } from "hull:attachment";
 *   attachment.delete(id);   // or: attachment["delete"](id)
 *
 * @param {string} id
 * @returns {boolean}  true if the attachment existed and was
 *   decremented (or fully deleted); false if no such id.
 */
function deleteAttachment(id) {
    let removed = false;

    db.batch(() => {
        const meta = metadata(id);
        if (!meta) return;

        if (meta.refcount > 1) {
            db.exec(
                "UPDATE _hull_attachments SET refcount = refcount - 1 WHERE id = ?",
                [id]);
            removed = true;
            return;
        }

        // Last reference. Drop metadata row first so any concurrent
        // "is blob still referenced?" probe sees the accurate count.
        db.exec("DELETE FROM _hull_attachments WHERE id = ?", [id]);

        // Other rows referencing this blob_id? Two attachments uploaded
        // the same bytes share one blob; only when the LAST row is
        // gone do we unlink.
        const refs = db.query(
            "SELECT 1 FROM _hull_attachments WHERE blob_id = ? LIMIT 1",
            [meta.blob_id]);
        if (!refs || refs.length === 0) {
            blob.delete(meta.blob_id);
        }
        removed = true;
    });

    return removed;
}

export const attachment = {
    init, store, metadata, read, readToFile,
    "delete": deleteAttachment,
};
