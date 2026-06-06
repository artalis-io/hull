# `hull/blob@1` — Content-Addressed Storage

**Status:** Design locked — implementation pending (§1.5.b-3.5).

## Overview

`hull/blob@1` is a pure content-addressed disk storage primitive. Take
bytes (or a stream), get back a SHA-256-keyed ID. Read back by ID.
That's it.

Other modules layer policy on top — naming, refcounting, auth, MIME
validation, eviction strategy. blob owns none of those concerns.

## Goals

- Streaming put with **on-the-fly SHA-256**. Bytes flow through the
  hasher in lockstep with the temp-file write — never buffered just to
  compute the hash, never re-read just to hash.
- Atomic writes via temp + `rename(2)` — readers never see partial bytes.
- Sharded directory layout — scales to tens of millions of blobs.
- Self-verifying — filename IS the SHA-256 of the contents; `fsck` is
  one line of shell.
- Zero `HL_ENABLE_DB` dependency — works in compute-only builds, which
  is exactly where the compute AOT cache and Lua bytecode cache matter
  most.
- Idempotent — concurrent puts of identical bytes both succeed; second
  rename is a no-op overwrite of identical bytes; no locking needed.

## Non-goals

| Out of scope | Why | Where it lives instead |
|---|---|---|
| Naming | A pure CAS doesn't know your tool's version string or your attachment's `original_name`. | Caller's metadata layer (`_hull_tools.json`, `_hull_attachments` table, etc.) |
| Refcounting | Caller decides when bytes can be deleted. | `attachment.delete(id)` decrements a refcount in its own table, calls `blob.delete()` at 0 |
| Compression | Pre-compress before `put()` if you want; the resulting hash differs. | Application code |
| Encryption at rest | See [Non-goal: encryption at rest](#non-goal-encryption-at-rest) below for the full rationale. | `hull/web/attachment@1` (or future per-consumer modules) |
| Replication / remote storage | Local disk only in v1. | Future `HlBlobBackend` vtable if S3 backing ever becomes a need |
| TTL / auto-eviction | Some callers (attachment) never want eviction; others (LLM cache) want LRU. | Opt-in via `blob.cleanup({...})` called from `app.daily(...)` |

## Non-goal: encryption at rest

Blob does not encrypt stored bytes. The reason is sharper than YAGNI —
**encryption-at-rest fundamentally breaks content addressing**, which
is blob's entire reason to exist.

| Problem | Detail |
|---|---|
| **Random-nonce encryption loses dedup.** | `blob.put(bytes)` of identical plaintext from two callers would produce different ciphertexts → different SHA → no shared storage. Content addressing becomes pointless. |
| **Convergent encryption** (deriving the nonce from the plaintext SHA so identical inputs encrypt identically) preserves dedup but introduces a confirmation-of-file attack: an attacker who can guess the plaintext can encrypt their guess and check whether that ID is stored. Used by Tahoe-LAFS and some backup systems, but the security caveats are non-trivial and the trade-off is only the right call when the application understands them. |
| **Encryption is data-shaped, not storage-shaped.** | The decision "this file should be encrypted" belongs at the layer that knows what the file IS — `attachment.store()` knows it's a user upload; the compute AOT cache knows it's a public derived artifact. Blob just sees bytes and has no basis to make that call. |
| **Key management is out of scope for a storage primitive.** | A key-in-env-var is barely better than filesystem ACLs; an HSM / Vault integration belongs in its own module. Either way, not blob's problem. |
| **Hull's deployment model is single-binary, single-machine.** | The threat that blob-layer encryption uniquely addresses (disk-read without process-read) is unusual: if disk is compromised, RAM usually is too. |

### Where encryption belongs: at the consumer layer

If `hull/web/attachment@1` (or any future consumer) needs at-rest
encryption, the pattern is:

```
1. Caller computes plaintext SHA-256 → metadata.plaintext_sha  (dedup key)
2. Caller encrypts plaintext with its application key → ciphertext
3. Caller calls blob.put_stream(ciphertext) → blob_id  (ciphertext SHA)
4. Metadata table stores both: { id, plaintext_sha, blob_id, ... }
5. On read: blob.get(blob_id) → ciphertext → decrypt with same key → plaintext
```

Properties this preserves:

- **App-layer dedup still works.** Two users uploading the same file
  share a `_hull_attachments` row keyed by `plaintext_sha` — and
  consequently share the same encrypted blob, because the encryption
  is keyed (deterministic on the same key + plaintext) at the
  application boundary.
- **Blob layer stays oblivious.** Blob sees random-looking ciphertext
  bytes and content-addresses them; nothing changes about its
  semantics or its existing test surface.
- **Key management lives where the keys live.** The application
  module (attachment) owns the relationship between its key, its
  metadata, and the storage layer below. Blob inherits nothing.

`hull/web/attachment@1` does NOT ship with at-rest encryption in
v0.1.9. The hook to add it later is clean: extend the metadata schema
with `encryption_key_id` / `nonce` columns when a real consumer
requires it; the storage interface beneath doesn't change.

## Storage layout

```
<root>/
├── blobs/
│   ├── 00/
│   │   ├── 00f1c8...sha256
│   │   └── 003a72...sha256
│   ├── 01/
│   ├── ...
│   └── ff/
└── tmp/
    └── .blob-<random>.tmp    # in-flight writes; init sweeps stale ones
```

- **1-level shard by default (256 subdirs).** Good up to ~25M total
  blobs before any shard hits inode-density issues on common
  filesystems. `shard_depth = 2` opt-in for unusually large stores
  (65K dirs, ~6B blob ceiling).
- **Filename is the full lowercase hex SHA-256.** Self-verifying —
  `sha256(file) == basename(file)` always.
- **`tmp/` is a sibling, not under `blobs/`.** Keeps `blob.iter()`
  from seeing in-flight writes; same filesystem as `blobs/` so atomic
  rename works without `EXDEV`.
- **No metadata files.** Size + atime come from filesystem `stat`;
  hash is in the filename. LRU policy uses filesystem `mtime`/`atime`,
  updated on read via a single `utimes(2)` (opt-out per-call via
  `track_access = false`).

## API surface

### Lua

```lua
local blob = require("hull.blob")

-- Initialization — validates dir against manifest fs.write allowlist;
-- sweeps tmp/* older than tmp_max_age; creates shard dirs lazily.
blob.init({
    dir         = "data/blobs",      -- required
    shard_depth = 1,                 -- optional: 1 (default) or 2
    tmp_max_age = 3600,              -- seconds; default 1h
})

-- Whole-buffer put (small blobs)
local id, size = blob.put(bytes)                          -- bytes = Lua string
local id       = blob.put_verified(bytes, expected_id)    -- raises on SHA mismatch

-- Streaming put (large blobs) — SHA computed incrementally
local w = blob.writer()                          -- or blob.writer({ expected = "..." })
w:write(chunk1)
w:write(chunk2)
local id, size = w:finalize()                    -- atomic rename; w is now closed
-- OR
w:abort()                                        -- removes tmp file

-- Reads
local bytes = blob.get(id)                       -- nil if missing
local bytes = blob.get(id, { track_access = false })   -- skip the utimes() call
local r     = blob.reader(id)                    -- nil if missing
while true do
    local chunk = r:read(65536)
    if not chunk then break end
    -- process chunk
end
r:close()

-- Metadata (single stat call each)
blob.exists(id)              -> bool
blob.size(id)                -> integer or nil
blob.atime(id)               -> integer or nil       -- unix timestamp

-- Deletion
blob.delete(id)              -> bool                 -- true if removed

-- Enumeration (for ops, scrub, eviction)
for id, size in blob.iter() do ... end
blob.total_size()            -> integer
blob.count()                 -> integer

-- Opt-in eviction
blob.cleanup({
    max_total_size = 10 * 1024^3,                    -- 10 GiB
    max_age        = 30 * 86400,                     -- 30 days
    strategy       = "lru",                          -- or "fifo"
    dry_run        = false,
}) -> { removed = N, freed_bytes = B, kept = M }
```

### JavaScript

```javascript
import { blob } from "hull:blob";

blob.init({ dir: "data/blobs", shardDepth: 1, tmpMaxAge: 3600 });

const { id, size } = blob.put(bytes);                  // bytes = ArrayBuffer
const id2 = blob.putVerified(bytes, expectedId);

const w = blob.writer();                               // or { expected: "..." }
w.write(chunk1);
w.write(chunk2);
const { id, size } = w.finalize();

const bytes = blob.get(id);                            // ArrayBuffer or null
const bytes2 = blob.get(id, { trackAccess: false });
const r = blob.reader(id);
let chunk;
while ((chunk = r.read(65536)) !== null) { /* ... */ }
r.close();

blob.exists(id);  blob.size(id);  blob.atime(id);  blob.delete(id);
for (const { id, size } of blob.iter()) { /* ... */ }
blob.totalSize();  blob.count();
blob.cleanup({ maxTotalSize, maxAge, strategy, dryRun });
```

## C cap layer (`src/hull/cap/blob.h`, `src/hull/cap/blob.c`)

```c
typedef struct HlBlob HlBlob;

/* Lifecycle */
int  hl_cap_blob_init(HlBlob **out, const char *dir, int shard_depth,
                        uint64_t tmp_max_age_sec);
void hl_cap_blob_free(HlBlob *b);

/* Write paths. `expected` may be NULL; if non-NULL, finalize fails
 * on SHA mismatch. SHA-256 is computed on the fly during the write
 * loop — bytes flow through the hasher in lockstep with the temp
 * file write. */
int  hl_cap_blob_put(HlBlob *b, const uint8_t *buf, size_t len,
                       const char *expected, char *out_id);

typedef struct HlBlobWriter HlBlobWriter;
int  hl_cap_blob_writer_open(HlBlob *b, const char *expected,
                                HlBlobWriter **out);
int  hl_cap_blob_writer_write(HlBlobWriter *w, const uint8_t *buf, size_t len);
int  hl_cap_blob_writer_finalize(HlBlobWriter *w, char *out_id, size_t *out_size);
void hl_cap_blob_writer_abort(HlBlobWriter *w);

/* Read paths */
int  hl_cap_blob_get(HlBlob *b, const char *id, int track_access,
                       uint8_t **out_buf, size_t *out_len);
typedef struct HlBlobReader HlBlobReader;
int  hl_cap_blob_reader_open(HlBlob *b, const char *id, int track_access,
                                HlBlobReader **out);
int  hl_cap_blob_reader_read(HlBlobReader *r, uint8_t *buf, size_t cap,
                                size_t *out_len);
void hl_cap_blob_reader_close(HlBlobReader *r);

/* Metadata */
int  hl_cap_blob_exists(HlBlob *b, const char *id);
int  hl_cap_blob_stat(HlBlob *b, const char *id, size_t *size, int64_t *atime);
int  hl_cap_blob_delete(HlBlob *b, const char *id);

/* Enumeration */
typedef int (*HlBlobIterCb)(const char *id, size_t size, void *user);
int  hl_cap_blob_iter(HlBlob *b, HlBlobIterCb cb, void *user);

/* Eviction */
typedef struct {
    uint64_t max_total_size;
    uint64_t max_age_sec;
    enum { HL_BLOB_LRU, HL_BLOB_FIFO } strategy;
    int      dry_run;
} HlBlobCleanupOpts;
int  hl_cap_blob_cleanup(HlBlob *b, const HlBlobCleanupOpts *opts,
                           int *removed, uint64_t *freed);
```

All file I/O routes through `hl_cap_fs_*` for path validation against
the app's `manifest.fs.write` allowlist. SHA-256 reuses the
incremental hasher introduced in §1.5.b-2 (`crypto.create_sha256`'s
internal `hl_cap_crypto_sha256_*` API).

## Manifest contract

```lua
app.manifest({
    modules = { "hull/blob@1" },
    fs      = { write = { "data/blobs/" } },
})
```

Module load fails if `fs.write` doesn't cover the directory passed to
`blob.init()`. Same path-validation gate as `fs.mmap`'s `fs.read`
requirement.

## Build-flavor compatibility

Blob compiles cleanly under every flavor:

| Flag | Effect on blob |
|---|---|
| `HL_ENABLE_DB=0` | **No effect.** Blob has zero SQLite dependency. |
| `HL_ENABLE_HTTP_SERVER=0` | **No effect.** Blob is callable from `app.main(fn)` CLI flows. |
| `HL_ENABLE_HTTP_CLIENT=0` | **No effect.** No network. |
| `HL_ENABLE_WASM=0` | **No effect.** No compute coupling. |
| `HL_ENABLE_TUI=0` | **No effect.** |

In particular, a `HL_ENABLE_DB=0` compute-only build still gets the
full blob primitive — which is exactly where the compute AOT cache
and Lua bytecode cache live.

## Invariants

1. **Reads never see partial writes.** All writes go through
   `tmp/<random>.tmp` + atomic `rename(2)` to the final blob path.
   `rename` is atomic on the same filesystem; init enforces `tmp/`
   and `blobs/` are siblings.
2. **Filename = SHA-256 of contents.** Always re-verifiable.
   `blob.scrub({ verify = true })` (future ops helper) re-hashes
   every blob and reports mismatches.
3. **Idempotent put.** Two concurrent puts of identical bytes both
   succeed; second `rename` is a no-op overwrite of identical bytes.
   No locking needed.
4. **Cross-filesystem fallback.** If `rename` fails with `EXDEV` (rare
   — only when `dir` straddles a mount), fall back to copy + unlink.
5. **Init sweeps stale tmps.** Files in `tmp/` older than `tmp_max_age`
   (default 1h) are deleted on `blob.init()`. Prevents unbounded
   growth from crashed writers.
6. **No fsync on every write.** Performance > durability for a cache
   layer. Callers needing durability can call a future `blob.sync()`;
   hardware-fail = re-fetch / re-compute.
7. **`atime` updates are opt-out per call.** Default ON so LRU works
   correctly; `{ track_access = false }` for hot read paths
   (e.g. stdlib bytecode lookups).

## Migration map — existing implementations

### 1. `hull tools install` (`src/hull/tools_install.c`) — **migrate**

**Today:** Downloads `hull-<tool>-<platform>` from a GitHub release,
verifies SHA-256 against the signed release manifest, writes to
`~/.hull/tools/<tool>`. Lookup via `hl_tools_lookup_path()`.

**After migration:** Same download + SHA verification, but
`blob.put_verified(bytes, expected_sha)` stores under
`~/.hull/blobs/`. A small JSON sidecar at `~/.hull/tools/index.json`
maps `<tool>-<version>-<platform>` → blob_id. `hl_tools_lookup_path()`
resolves via the JSON.

**Why JSON, not SQLite:** tools_install runs in every build flavor
including `HL_ENABLE_DB=0`. JSON sidecar keeps it dependency-free.

**Benefits:**
- Uninstall = JSON delete + (optional) `blob.delete()` if no other
  refs. (Currently uninstall just `unlink()`s the named file.)
- Dedup if multiple tool versions share a binary (unlikely, but free).
- Consistent layout with other consumers.

**Compatibility:** Provide a symlink shim from `~/.hull/tools/<name>`
→ `~/.hull/blobs/<sha>` for one release as a bridge for tools-aware
scripts, then drop the symlinks.

**Risk:** none beyond the symlink shim's deprecation window.

### 2. Compute AOT cache (`stdlib/lua/hull/compute_build.lua`) — **migrate**

**Today:** `hull build` invokes `wamrc` to compile
`compute/<name>.wasm` → `compute/<name>.aot.<arch>`. Output lives next
to source in the app tree. Rebuild trigger = source mtime.

**After migration:** Key the AOT output by `(sha256(wasm_source),
arch_tag)`. `blob.put` the AOT bytes; JSON sidecar at
`~/.hull/cache/compute/index.json` maps `(source_sha, arch)` →
blob_id. Build step: if blob exists, skip wamrc; else compile + store.
Cache lives system-wide under `~/.hull/cache/` so multiple apps using
the same WASM module share the artifact.

**Why JSON, not SQLite:** compute exists in `HL_ENABLE_DB=0` builds.

**Benefits:**
- Identical sources across apps share AOT artifacts.
- mtime-based staleness → content-based correctness.
- Iterate on one compute module without re-AOT'ing others.
- `--no-aot` still works — skips the lookup AND the compile.

**Compatibility:** `compute/<name>.aot.<arch>` in-app artifacts
continue to be honored (loaded directly if present). The new cache is
additive.

**Risk:** Cache can grow unbounded over time on a developer machine.
Mitigate with `hull cache prune` subcommand wrapping
`blob.cleanup({ max_total_size = 1 * 1024^3 })`.

### 3. `hull/web/attachment@1` (§1.5.b-4) — **new, built on blob**

attachment becomes thin: `_hull_attachments` metadata table (id,
blob_id, original_name, mime, declared_mime, size, uploaded_by,
uploaded_at, refcount) +
`attachment.store(part) -> id` calls `blob.writer()` while streaming +
sniffs MIME on the first chunk via `hl_cap_mime_sniff` + writes
metadata row + `attachment.serve()` route helper looks up metadata +
streams `blob.reader(blob_id)` to the response. attachment owns the
refcount; blob doesn't know there's one.

**Why SQLite here, not JSON:** attachment is web+DB-scoped (lives
under `hull/web/*`); it depends on `hull/db@1` anyway and won't exist
in `HL_ENABLE_DB=0` builds.

**Refcount semantics:** `attachment.delete(id)` decrements; when
refcount = 0, sets `pending_gc = true`. `attachment.cleanup()` finds
`pending_gc` rows older than 24h and calls `blob.delete(blob_id)`
THEN removes the row.

### 4. Lua bytecode cache — **new consumer (v0.1.10+)**

`string.dump(load(source))` produces bytecode bytes. Cache key =
`sha256(source)`. Apply when loading large stdlib modules or user
templates. No sidecar map needed — the cache lookup IS the blob ID
derived from the source SHA, so just `blob.get(sha256(source))`.

Hot path: pass `{ track_access = false }` to avoid the `utimes`
syscall on every hot stdlib lookup.

### 5. LLM artifact cache — **new consumer (latent, v0.1.11+)**

Cache key = `sha256(prompt + context + model_id + temperature_str)`.
TTL via `blob.cleanup({ max_age = 7 * 86400, strategy = "lru" })`
called from `app.daily(...)`. No sidecar needed if the SHA derivation
is canonical.

### 6. Template AST cache (dev mode) — **migrate (v0.1.10+)**

**Today:** `stdlib/lua/hull/template.lua` compiles templates to Lua
source via codegen, caches the compiled function in-process. Cache
cleared on `hull dev` reload.

**After migration:** Persist the compiled Lua source (or
`string.dump()` bytecode) keyed by `sha256(template_source)`.
Reload-survival; cross-process reuse. The in-process function cache
stays — blob is the persistence layer underneath it. No sidecar
(cache key derives from blob ID).

### 7. `hull update` artifact staging — **possible future (v0.1.12+)**

`hull update` downloads new binaries to `/tmp/hull.new` then atomic-
renames over the running binary. Could route through blob for safer
staging + rollback. Out of scope for v0.1.9.

## Tests

| Suite | What it covers |
|---|---|
| `tests/hull/cap/test_blob.c` | C-layer: put/get roundtrip, streaming put (SHA-on-the-fly correctness), streaming get, atomic rename, tmp cleanup on init, `EXDEV` fallback (skipped if not testable), `put_verified` mismatch raises, concurrent put of same bytes, iter accuracy, cleanup LRU vs FIFO, manifest path validation, `track_access = false` doesn't update atime |
| `tests/hull/test_blob_lua.c` | Lua binding integration: round-trip, writer abort, reader.close idempotent, exists/size/atime, cleanup dry_run reports without removing |
| `tests/hull/test_blob_js.c` | Same for JS |
| `tests/e2e_blob.sh` | End-to-end from a live `hull dev` app: put + get + iter + cleanup, manifest enforcement (declare wrong dir → init fails) |

## Sequencing within §1.5.b (v0.1.9)

```
b-1   Streaming multipart parser in Keel              ✓ done (v2.0.0→v2.2.0)
b-2   Hull-side iterator bindings + create_sha256     ✓ done
b-3   MIME sniffer (hl_cap_mime_sniff)                ✓ done
b-3.5 hull/blob@1                                     ← next
b-4   hull/web/attachment@1   (built on b-3 + b-3.5)
b-5   Photo upload demo in examples/hypermedia_todo   (consumes b-4)
b-6   Docs: docs/attachments.md + docs/htmx.md upload section
```

### §1.5.b-3.5 scope estimate

| File | Lines |
|---|---|
| `include/hull/cap/blob.h` | ~80 (struct decls + 14 function sigs + cleanup opts) |
| `src/hull/cap/blob.c` | ~500 (init/put/writer/reader/get/iter/cleanup, on-the-fly hashing) |
| `src/hull/runtime/lua/mod_blob.c` | ~280 |
| `src/hull/runtime/js/mod_blob.c` | ~310 |
| `src/hull/module_registry.c` | +1 entry (`hull/blob`) |
| `tests/hull/cap/test_blob.c` | ~450 |
| `tests/e2e_blob.sh` | ~180 |
| `stdlib/context/blob.md` | ~180 (minimal/compact/full) |
| `docs/blob.md` | this file ✓ |

Migrations 1 (tools) and 2 (compute AOT) land in **v0.1.10**, after
blob is shipped + battle-tested by §1.5.b-4 in v0.1.9.

## See also

- `docs/multipart.md` — the multipart iterator that produces the
  `Part` objects attachment consumes
- `examples/multipart_upload/` — Lua + JS demo of the iterator + the
  incremental SHA-256 hasher
- `src/hull/cap/mime.h` — the MIME sniffer attachment uses
- `docs/roadmap_next.md §1.5.b` — full milestone scope
