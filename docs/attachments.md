# Attachments

`hull/attachment@1` is content-addressed attachment storage built on
`hull/blob@1`. Each `attachment.store(part)` streams a multipart
`Part` into a blob and inserts a metadata row keyed by a fresh
attachment id. The blob layer dedupes on disk automatically (two
users uploading the same JPEG share one on-disk blob but get
distinct attachment ids); the metadata layer tracks who owns what
and refcounts so deletes are correct.

Two modules:

| Module | Where | What |
|---|---|---|
| `hull/attachment@1` | flat top-level | core storage: store, metadata, read, read_to_file, delete |
| `hull/web/attachment-serve@1` | `hull/web/*` | auth-gated HTTP response helper (`serve(req, res, id, opts)`) |

The split is deliberate. The core is FS + DB only - no HTTP
coupling - so CLI tools and batch jobs can use it. Only `serve`
depends on `hull/http-server`.

## Manifest declaration

```lua
app.manifest({
    modules = {
        "hull/attachment@1",
        "hull/web/attachment-serve@1",   -- if you'll serve over HTTP
        "hull/blob@1",
        "hull/db@1",
        "hull/fs@1",
        "hull/mime@1",
        "hull/time@1",
    },
    fs = { write = { "data/" } },        -- where blob.init will mkdir
})
```

```javascript
app.manifest({
    modules: [
        "hull/attachment@1",
        "hull/web/attachment-serve@1",
        "hull/blob@1",
        "hull/db@1",
        "hull/fs@1",
        "hull/mime@1",
        "hull/time@1",
    ],
    fs: { write: ["data/"] },
});
```

## Initialisation

`blob.init` + `attachment.init` must run AFTER the sandbox wires the
fs capability - wrap in `app.main`:

```lua
local attachment = require("hull.attachment")
local blob       = require("hull.blob")

app.main(function()
    blob.init({ dir = "data/blobs" })
    attachment.init({
        max_size = 4 * 1024 * 1024,
        mime_allowlist = { "image/png", "image/jpeg",
                           "image/gif", "image/webp" },
    })
end)
```

```javascript
import { attachment } from "hull:attachment";
import { blob }       from "hull:blob";

app.main(() => {
    blob.init({ dir: "data/blobs" });
    attachment.init({
        maxSize: 4 * 1024 * 1024,
        mimeAllowlist: ["image/png", "image/jpeg",
                        "image/gif", "image/webp"],
    });
});
```

`init` is sticky: re-calling it with a subset of options preserves
previously-set values. Pass the full config in a single call.

## API surface

| Lua | JS | What |
|---|---|---|
| `attachment.init(opts)` | `attachment.init(opts)` | One-time setup (creates `_hull_attachments` table; stores limits) |
| `attachment.store(part, opts)` | `attachment.store(part, opts)` | Stream a multipart `Part` into storage. Returns attachment id. |
| `attachment.metadata(id)` | `attachment.metadata(id)` | Returns the metadata row, or `nil`/`null` if missing. |
| `attachment.read(id)` | `attachment.read(id)` | Materialise full bytes in memory. Returns `nil`/`null` if missing. |
| `attachment.read_to_file(id, dst)` | `attachment.readToFile(id, dst)` | Stream to a file path under `fs.write`. |
| `attachment.delete(id)` | `attachment.delete(id)` | Decrement refcount; queue the blob for unlink if last reference. |
| `attachment.sweep(opts)` | `attachment.sweep(opts)` | Unlink queued blobs older than `grace` (default `orphan_grace`) that no row references. Returns the count. |

Naming: JS is camelCase, Lua is snake_case. `attachment.delete` works
in both runtimes (JS exposes it via a bracket-key export since
`delete` is an operator keyword in expressions but valid as a
property name).

## Storage layout

After `init`:

```
data/
└── blobs/
    └── blobs/
        ├── <ab>/<cd>/<full-sha256>           ← one file per unique content
        └── …
```

The double `blobs/blobs/` is `blob.init({ dir = "data/blobs" })`
plus the blob store's own internal sharding root. Shard depth
defaults to 1 (first two hex chars of the SHA); pass
`shard_depth = 2` to `blob.init` for two levels (large stores).

SQLite tables:

| Table | Owner | Purpose |
|---|---|---|
| `_hull_attachments` | hull/attachment | per-attachment metadata + refcount |
| _your_join_table_ | your app | links attachments to whatever owns them |

Direct access to `_hull_attachments` from user code is **blocked**
by the capability layer (it's an internal hull table). Always go
through `attachment.metadata(id)` to read it.

## MIME validation

`attachment.store` enforces validation via two channels:

1. **Sniffed MIME** - `mime.sniff()` reads the first non-empty
   chunk's magic bytes (PNG header `89 50 4E 47`, JPEG SOI `FF D8`,
   etc.). If a `mime_allowlist` is configured, the sniffed value
   must be in it.
2. **Declared MIME** - the multipart Part's `Content-Type` header.
   Recorded as `declared_mime` in the metadata row for audit, but
   **never trusted** for the allowlist gate (clients can spoof it).

Both are stored separately. Apps that need to flag mismatches (e.g.
`Content-Type: image/png` but sniffed `application/pdf`) can compare
`metadata.mime` vs `metadata.declared_mime` after `store` returns.

For very bursty connections where the multipart parser might deliver
a tiny (<8 byte) first chunk, the sniffer falls back to
`application/octet-stream` - which fails the allowlist if one is
configured. Callers that need bullet-proof sniffing on those inputs
should buffer the first 512 bytes themselves before constructing a
Part-like object for `store`.

## Refcount semantics

`store` always returns a **fresh** attachment id with `refcount=1`.
Two uploads of identical bytes get **two distinct ids** but share
one on-disk blob (the blob layer dedupes by SHA-256).

`delete(id)`:
- decrements refcount on the metadata row
- at 0: removes the row AND, if no other row references the same
  `blob_id`, queues the blob in `_hull_attachment_orphans` - in the
  same transaction as the row delete
- then runs `attachment.sweep()`, which unlinks queued blobs that have
  waited `orphan_grace` seconds (`attachment.init({ orphan_grace = N })`,
  `orphanGrace` in JS; default 300, must be > 0) and that still no row
  references, each re-checked in its own transaction. The inline sweep is
  best-effort: an unlink that fails (EACCES, EROFS) leaves its queue entry
  for a later sweep and never makes `delete` raise after the delete itself
  succeeded

Why deferred: `delete` may run inside the app's own `db.batch`, where
its transaction is only a SAVEPOINT. Unlinking "after our commit" then
unlinked before the app's real COMMIT, and an app rollback restored a
row whose blob was gone (410 for good). Queued, the rollback takes the
queue entry with it. `orphan_grace` must therefore outlast the longest
app transaction that deletes attachments (the sweep cannot tell an
uncommitted delete from a committed one). Schedule `attachment.sweep()`
(e.g. `app.every`) if deletes are rare and you want the disk back
promptly; `sweep({ grace = 0 })` unlinks everything queued.

`store` finalises the content-addressed blob before inserting its row,
so a sweep in another process could unlink a blob a concurrent upload
just deduplicated onto. `store` re-checks the blob exists after its
insert and, if it was reclaimed, removes the row and raises ("retry the
upload") instead of leaving a row pointing at nothing. A row inserted
inside an app transaction is invisible to another process until it
commits, so that window is only closed while it stays shorter than
`orphan_grace` past the delete that queued the blob.

When the metadata INSERT itself fails, `store` queues the new blob for
the sweep before raising (the sweep keeps it if another row shares it).
Not covered: a `store` inside an app `db.batch` that later rolls back.
The row and any queue entry go with the rollback, and the blob stays on
disk unreferenced; store uploads outside a transaction that may roll
back, or delete the attachment explicitly on the failure path.

## Serving over HTTP

`attachment.serve(req, res, id, opts)` is the auth-gated response
helper. Lives in the separate `hull/web/attachment-serve@1` module:

```lua
local attachment_serve = require("hull.web.attachment-serve")

app.get("/files/:id", function(req, res)
    attachment_serve.serve(req, res, req.params.id, {
        auth_check = function(req, meta)
            -- Return true to allow. Anything else (nil, false, missing
            -- function) responds 403 - fail-closed.
            return req.ctx.user_id == meta.uploaded_by
        end,
    })
end)
```

```javascript
import { attachmentServe } from "hull:web:attachment-serve";

app.get("/files/:id", (req, res) => {
    // serve() always returns a Promise: RETURN (or await) it. With an
    // async authCheck the response is written only when the check
    // settles; a dropped Promise ends the request empty.
    return attachmentServe.serve(req, res, req.params.id, {
        authCheck: (req, meta) => req.ctx.user_id === meta.uploaded_by,
    });
});
```

What `serve` sets on the wire:

- `Content-Type: <metadata.mime>` (sniffed MIME, not declared)
- `Content-Disposition: attachment; filename="<ascii-fallback>"; filename*=UTF-8''<percent-encoded>`
  - RFC 5987 encoding with both an ASCII fallback (`é` → `_` etc.)
    and the UTF-8-percent-encoded form. Browsers save the file
    with the original upload name, including non-ASCII characters
    (Japanese, emoji, accented Latin all work).
- `ETag: "<full-64-hex-blob-id>"` - strong ETag. Because the blob
  layer is content-addressed by SHA-256, the blob id IS a
  genuine cryptographic fingerprint of the bytes, so strong
  validation (vs `W/"..."` weak) is correct here.

Response codes:

| Status | When |
|---|---|
| **200** | auth allowed, blob present |
| **304** | client's `If-None-Match` includes the current ETag |
| **403** | `auth_check` omitted, wasn't a function, or returned anything but exactly `true` (JS: or a Promise not resolving to `true`) |
| **500** | JS: the `authCheck` Promise rejected |
| **404** | no metadata row for that id |
| **410** | metadata says the blob exists but `blob.get` can't find it (signal to caches to drop their copy) |

The `auth_check` function receives `(req, metadata)` - `metadata` is
the live row so you can gate on `uploaded_by`, mime, size, etc.

## Linux Landlock footgun

On Linux, `unveil(2)` (via the Landlock polyfill) rejects paths
that don't exist on disk yet. This means a `fs.write = { "data/" }`
declaration in your manifest will silently produce an empty write
allowlist if `./data/` doesn't exist when the sandbox phase fires -
and then `blob.init`'s `mkdir` of `data/blobs/` fails inside the
sandbox.

macOS Seatbelt is permissive about this; the bug only surfaces on
Linux. For now the fix is `mkdir -p data` in your deploy script
before launching hull. The proper fix - having hull's sandbox layer
mkdir declared `fs.write` paths before unveiling - is tracked as a
separate enhancement.

## See also

- [`docs/htmx.md` § Photo uploads](htmx.md) - client-side upload patterns
- [`docs/blob.md`](blob.md) - the underlying content-addressed store
- [`examples/hypermedia_photos`](../examples/hypermedia_photos) - working end-to-end demo
