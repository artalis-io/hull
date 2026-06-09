--- Content-addressed attachment storage backed by hull/blob + SQLite metadata.
--
-- @module hull.attachment
-- @license AGPL-3.0-or-later
--
-- Thin layer over @{hull.blob}: each `attachment.store(part)` streams a
-- multipart `Part` through `blob.writer()` (content-addressed, dedupes
-- on disk automatically) and inserts a metadata row in
-- `_hull_attachments` (id, blob_id, original_name, mime, declared_mime,
-- size, uploaded_by, uploaded_at, refcount).
--
-- The attachment id is fresh per upload — refcount tracks how many
-- callers own it. `attachment.delete(id)` decrements; at 0, the
-- metadata row is removed and, if no other row references the same
-- `blob_id`, the on-disk blob is deleted via `blob.delete()`
-- synchronously (no deferred GC — kept the module small and the
-- behavior obvious; an undo grace period can be added later if a use
-- case appears).
--
-- Lives flat at `hull/attachment` rather than under `hull/web/` — the
-- core API (store / read / metadata / delete) is FS + DB only, with
-- no HTTP dependency. The web-specific auth-gated
-- `serve(req, res, id, { auth_check })` helper lives in the separate
-- `hull/web/attachment-serve@1` module.
--
-- Portable SQL: `?` placeholders, INTEGER timestamps + refcount,
-- TEXT ids, no AUTOINCREMENT, no PRAGMAs — the schema works as-is
-- on the future Postgres backend.

local blob = require("hull.blob")
local crypto = require("hull.crypto")
local db = require("hull.db")
local mime_mod = require("hull.mime")
local time = require("hull.time")

local attachment = {}

-- Module-level config set by attachment.init.
local _max_size = nil          -- nil = unlimited (per upload)
local _mime_allowlist = nil    -- nil = any MIME allowed; otherwise set<string>

--- Initialize attachment storage.
--
-- Creates the `_hull_attachments` table if absent and stores module-
-- level limits. Must be called once at startup before any other
-- attachment function. The underlying `hull/blob` store must be
-- initialized separately via `blob.init({ dir = "..." })`.
--
-- @tparam[opt] table opts
--   `max_size` — per-attachment byte cap (integer, default unlimited).
--   `mime_allowlist` — array of allowed sniffed-MIME strings
--     (default unlimited). Sniffed MIME is checked first; declared
--     `Content-Type` is recorded separately for audit but NOT trusted
--     for the allowlist.
--
-- Re-init semantics: only options explicitly present in the second
-- (or later) call are updated; omitted keys preserve their prior
-- value. Effectively "sticky" — call once with the full config.
function attachment.init(opts)
    opts = opts or {}
    if opts.max_size ~= nil then
        _max_size = opts.max_size
    end
    if opts.mime_allowlist ~= nil then
        _mime_allowlist = {}
        for _, m in ipairs(opts.mime_allowlist) do
            _mime_allowlist[m] = true
        end
    end

    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_attachments (
            id TEXT PRIMARY KEY,
            blob_id TEXT NOT NULL,
            original_name TEXT NOT NULL,
            mime TEXT NOT NULL,
            declared_mime TEXT,
            size INTEGER NOT NULL,
            uploaded_by TEXT,
            uploaded_at INTEGER NOT NULL,
            refcount INTEGER NOT NULL DEFAULT 1
        )
    ]])
    -- Index for "is this blob still referenced?" delete-path query.
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx__hull_attachments_blob_id
        ON _hull_attachments(blob_id)
    ]])
end

-- 32 hex chars from 16 random bytes — 128 bits of entropy. Plenty
-- for opaque attachment ids that aren't user-guessable.
local function generate_id()
    local raw = crypto.random(16)
    local hex = {}
    for i = 1, #raw do
        hex[i] = string.format("%02x", string.byte(raw, i))
    end
    return table.concat(hex)
end

--- Store a multipart part as an attachment.
--
-- Streams the part through `blob.writer()` (content-addressed) and
-- inserts a metadata row with `refcount=1`. The blob layer dedupes
-- on disk automatically — two stores of the same bytes share one
-- on-disk blob but get distinct attachment ids.
--
-- Sniffs the MIME type from the first chunk via `mime.sniff()` and
-- validates against the allowlist if `attachment.init` set one. The
-- declared `Content-Type` from the part is recorded separately
-- (`declared_mime`) but NOT trusted for allowlist enforcement —
-- clients can spoof it.
--
-- @tparam part part  A multipart Part from `req:multipart()`. Must
--   have `filename` (text fields raise).
-- @tparam[opt] table opts
--   `uploaded_by` — optional opaque caller identifier (string)
--     persisted to the metadata row for audit. The attachment layer
--     never interprets it.
-- @treturn string  The fresh attachment id (32-char hex).
-- @error On size cap exceeded, MIME not in allowlist, or part is not
--   a file upload.
--
-- Sniffer note: the MIME type is determined from the FIRST non-empty
-- chunk only. If the multipart parser delivers the first chunk in
-- fewer than ~8 bytes (rare in practice), the sniffer may not see
-- enough magic to identify the format and will fall back to
-- "application/octet-stream" — which then fails the allowlist if one
-- is configured. Callers that need bullet-proof sniffing on a very
-- bursty connection should buffer the first 512 bytes themselves
-- before constructing a Part-like object.
function attachment.store(part, opts)
    opts = opts or {}
    assert(part, "attachment.store: part required")
    if not part.filename then
        error("attachment.store: part is not a file upload (no filename)")
    end

    local declared = part.content_type or "application/octet-stream"

    local w = blob.writer()
    local size = 0
    local sniffed = nil
    local blob_id = nil

    -- Wrap the streaming write + finalize in pcall so ANY error path
    -- (chunks() raising, sniff() raising, write() raising, size cap,
    -- MIME allowlist, finalize size mismatch) reliably calls
    -- writer:abort() — without this, an exception from inside the
    -- iterator would leak the temp file. We also call blob.delete()
    -- if finalize already committed the blob before the
    -- size-mismatch check fires; the blob is content-addressed so
    -- deletion is safe even on dedup (we just decrement and the
    -- store handles the actual unlink).
    local ok, err = pcall(function()
        for chunk in part:chunks() do
            if sniffed == nil and #chunk > 0 then
                sniffed = mime_mod.sniff(chunk) or "application/octet-stream"
                if _mime_allowlist and not _mime_allowlist[sniffed] then
                    error("attachment.store: MIME not allowed: " .. sniffed)
                end
            end
            size = size + #chunk
            if _max_size and size > _max_size then
                error("attachment.store: PART_TOO_LARGE (size " .. size ..
                      " > max " .. _max_size .. ")")
            end
            w:write(chunk)
        end

        -- Empty parts: nothing to sniff; record as octet-stream and
        -- skip the allowlist (no content to gate).
        sniffed = sniffed or "application/octet-stream"

        local fid, blob_size = w:finalize()
        blob_id = fid
        if blob_size ~= size then
            error("attachment.store: blob writer size mismatch (" .. blob_size ..
                  " vs " .. size .. ")")
        end
    end)

    if not ok then
        -- abort() is a no-op after successful finalize, so this is
        -- safe regardless of which step in the pcall failed. The
        -- size-mismatch path however IS post-finalize — clean up the
        -- already-committed blob if we have its id.
        pcall(function() w:abort() end)
        if blob_id then
            pcall(function() blob.delete(blob_id) end)
        end
        error(err)
    end

    local id = generate_id()
    db.exec([[
        INSERT INTO _hull_attachments
        (id, blob_id, original_name, mime, declared_mime,
         size, uploaded_by, uploaded_at, refcount)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1)
    ]], { id, blob_id, part.filename, sniffed, declared,
          size, opts.uploaded_by, time.now() })

    return id
end

--- Fetch metadata for an attachment.
--
-- @tparam string id  Attachment id.
-- @treturn[1] table  `{id, blob_id, original_name, mime, declared_mime,
--   size, uploaded_by, uploaded_at, refcount}`.
-- @treturn[2] nil  When no attachment exists with that id.
function attachment.metadata(id)
    local rows = db.query([[
        SELECT id, blob_id, original_name, mime, declared_mime,
               size, uploaded_by, uploaded_at, refcount
        FROM _hull_attachments WHERE id = ?
    ]], { id })
    if not rows or #rows == 0 then return nil end
    return rows[1]
end

--- Read an attachment's bytes.
--
-- Materialises the whole blob in memory. For large files prefer
-- `attachment.read_to_file` (streams directly to disk; lands in
-- PR 2) or open a reader via `blob.reader(metadata.blob_id)` and
-- consume chunks.
--
-- @tparam string id  Attachment id.
-- @treturn[1] string  Raw bytes (binary-safe).
-- @treturn[2] nil  When no attachment exists with that id, or the
--   underlying blob is missing. JS parity: bare nil/null with no
--   reason (the missing-id-vs-missing-blob distinction isn't
--   actionable to callers and was an unintended divergence — see
--   PR 1 audit).
function attachment.read(id)
    local meta = attachment.metadata(id)
    if not meta then return nil end
    return blob.get(meta.blob_id)   -- nil pass-through if blob is gone
end

-- attachment.read_to_file / readToFile, attachment.delete, and
-- attachment.serve land in PR 2. The first brings hull/fs as a
-- (call-site-optional) dep that's cleanest to wire alongside the
-- web serve helper; the second + third are the GC + auth-gated
-- response slice.

return attachment
