# `hull/blob@1` — Content-Addressed Storage

**Status:** Design locked — implementation pending (§1.5.b-3.5).

> **Operator note.** This document is the design reference for the
> CAS primitive. If you're here to manage Hull's on-disk caches
> (`lua-bytecode`, `js-bytecode`, `compute-aot`, `templates`,
> `js-templates`, `tools`) — environment variables, sizing, isolation,
> deployment recipes — that's [docs/cache.md](cache.md).

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
local id, size = blob.put(bytes)                            -- bytes = Lua string
local id, size = blob.put(bytes, { durable = true })        -- fsync fd+dir
local id       = blob.put_verified(bytes, expected_id)      -- raises on SHA mismatch
local id       = blob.put_verified(bytes, expected_id, { durable = true })

-- Streaming put (large blobs) — SHA computed incrementally
local w = blob.writer()                          -- or { expected = "...", durable = true }
w:write(chunk1)
w:write(chunk2)
local id, size = w:finalize()                    -- atomic rename; w is now closed
-- OR
w:abort()                                        -- removes tmp file

-- put_verified short-circuit: when the caller provides `expected_id`
-- AND that blob already exists, blob.put skips the tmp+hash entirely
-- and returns the expected_id in ~2 µs (vs ~200 µs for the full
-- write-then-dedup path). Use for verified-install workflows where
-- the expected SHA came from a signed manifest.

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
const { id, size } = blob.put(bytes, { durable: true });
const id2 = blob.putVerified(bytes, expectedId);
const id3 = blob.putVerified(bytes, expectedId, { durable: true });

const w = blob.writer();                               // or { expected: "...", durable: true }
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

## Two C layers: low-level store + cap-layer wrapper

The implementation is split in two so that the bytecode cache, AOT
cache, future template-AST cache, and any other internal consumer
can share the atomic-rename + sharded-shard + LRU-cleanup
primitives without dragging in the manifest gate that exists only
to constrain *app* code.

```
                ┌─────────────────────────────────────┐
                │ App code                            │
                │   Lua:  require("hull.blob")        │
                │   JS:   import "hull:blob"          │
                └────────────────┬────────────────────┘
                                 │ manifest fs.write checked
                                 ▼
   include/hull/cap/blob.h ──── cap/blob.c (thin)
     HlBlob* (alias)              · hl_cap_blob_init: manifest gate
     hl_cap_blob_*                · all other entry points forward
                                 │
                                 ▼
   include/hull/blob_store.h  blob_store.c (low-level)
     HlBlobStore*                · pure CAS — no policy
     hl_blob_store_*             · atomic tmp+rename
                                 · sharded shards
                                 · LRU/FIFO cleanup
                                 · NEW: hl_blob_store_put_keyed
                                   (caller-supplied filename,
                                   bytes NOT content-hashed —
                                   for key-value caches)
                                 ▲
                                 │
                ┌────────────────┴────────────────────┐
                │ Runtime infrastructure              │
                │   src/hull/runtime/lua/             │
                │      bytecode_cache.c               │
                │   stdlib/cli/lua/hull/aot_cache.lua │
                │      via tool.blob_store_*          │
                └─────────────────────────────────────┘
```

`HlBlob` is now a typedef alias for `HlBlobStore`; all the
`hl_cap_blob_*` entry points are one-line forwarders that exist so
the cap layer can grow policy (audit emission, per-call permission
checks) without restructuring callers later.

### Per-store discipline: CAS *or* keyed, never both

`hl_blob_store_put` is content-addressed (filename =
`sha256(content)`). `hl_blob_store_put_keyed` is caller-keyed
(filename = caller-supplied 64-hex; bytes aren't hashed). Both
share the same atomic-rename + sharded layout, but **each store is
exclusively one or the other** — mixing them in the same root
breaks the "filename IS the SHA" invariant CAS callers rely on.

Apps' blob stores stay CAS. The runtime caches each get a
dedicated keyed store under `$HOME/.hull/blobs/runtime/<kind>/`.

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
6. **No fsync on the default put path** (cache-layer trade-off).
   Callers needing crash-survival use `blob.put(bytes, { durable =
   true })` / `blob.writer({ durable = true })` which fsync the tmp
   fd before close + fsync the shard directory after rename. The
   durable path costs ~12% throughput on APFS. Use for content that
   can't be cheaply re-derived (user uploads, release artifacts);
   keep default for re-derivable caches (compute AOT, bytecode, LLM).
7. **`atime` updates are opt-out per call.** Default ON so LRU works
   correctly; `{ track_access = false }` for hot read paths
   (e.g. stdlib bytecode lookups).

## Concurrency model

Blob's safety story relies on three properties, all of which are
provided by the underlying POSIX filesystem; blob adds no locks of its
own.

### Single-process

Hull runs the event loop on one thread, so concurrent blob calls from
within a single Hull process can't happen unless the worker pool
(`thread_pool`) is used. None of the current binding code dispatches
blob to the pool — all calls execute on the main thread. If a future
`blob.async.put` lands, it must coordinate via the worker queue (one
operation in flight per pool slot); blob itself is not reentrant.

### Multi-process (shared blob root)

When multiple Hull processes share a blob root (system-wide compute
AOT cache, `~/.hull/cache/compute/`; `hull tools install`; LLM cache),
the safety properties are:

| Scenario | Behavior |
|---|---|
| Concurrent put of identical bytes | Both writers write to distinct tmps (the `.blob-<16-hex>.tmp` suffix is from `crypto.random` — collisions cryptographically impossible). Both finalize: one renames, the other sees `stat(dest) == 0` and drops its tmp. **Safe — content is identical by SHA.** |
| Concurrent put + read of the same SHA | `rename(2)` is atomic on the same filesystem; the file at the blob path either exists with the full bytes or doesn't exist at all (never a partial-bytes state). Readers see all-or-nothing. |
| Concurrent delete + read | POSIX semantics: a reader holding an open fd can keep reading even after `unlink` removes the directory entry; the inode lives until the last fd closes. `blob.get`'s open-then-read sequence may see ENOENT if delete races between stat and open — returns -1, caller retries. |
| Concurrent cleanup + cleanup | Both processes collect snapshots independently and try to unlink the same files; first wins, second gets ENOENT (treated as "already gone", `removed_out` stays accurate). Safe but wasteful — for ops bins where this happens often, serialize via a per-host advisory lock above blob. |
| Concurrent cleanup + put | Cleanup's snapshot is materialized at call time; a put landing after `collect_entries` won't be in the snapshot, so it's never evicted by this pass. The new file is preserved as a side-effect of cleanup's snapshot semantics. |
| Concurrent cleanup + read | If cleanup unlinks while the reader has an open fd, POSIX keeps the inode alive; subsequent opens of the same id fail ENOENT (caller's problem — likely racing eviction with active use). |
| `EXDEV` fallback (tmp + final on different filesystems) | Non-atomic: `fopen` dest → loop `fread`/`fwrite` → `fclose` → `unlink` tmp. A crash between fclose and unlink leaves both files; next `blob.init()`'s stale-tmp sweep cleans up. **Practically rare** — init creates `tmp/` and `blobs/` as siblings under `root`, so EXDEV only triggers if `root` straddles a mount, which is unusual. |

### Weak spots

1. ~~**No `fsync`/`fdatasync` anywhere.**~~ **Resolved** via opt-in
   `{ durable = true }` on `put` / `writer`. Default path stays
   non-durable for cache workloads; durable path costs ~12%
   throughput.

2. ~~**`bump_atime` race.**~~ **Resolved** by switching from
   `utimes(path)` to `futimes(fd)`. The atime bump now binds to the
   exact inode the reader has open, eliminating the path-lookup race
   (file unlinked/renamed-over between open and utimes used to land
   the bump on a different inode or fail silently).

3. **No cross-process advisory lock for `cleanup`.** Two cleanup
   passes racing the same blob is correct (second unlink gets ENOENT,
   counts still accurate) but wasteful — twice the directory walks,
   twice the I/O. Blob deliberately doesn't take a lock because:
   (a) the common case is single-process (a daily `app.daily(...)`
   cleanup from one Hull process); (b) cross-process lock semantics
   vary across OSes (`flock` vs `fcntl(F_SETLK)`); (c) when multiple
   hosts share a blob root (NFS-style) the right answer is a
   higher-level coordinator (etcd, a leader-election sidecar), not
   `flock`. **If you do run cleanup from cron on the same host**,
   wrap the invocation:
   ```sh
   flock -n /var/lock/hull-blob-cleanup.lock -c "hull cleanup-blob ..."
   ```
   Adds a `/var/lock/hull-blob-cleanup.lock` sentinel; `flock -n`
   returns immediately when another holder is active so the duplicate
   cleanup is skipped, not queued.

4. **`iter()` snapshot memory is O(N).** ~80 bytes per blob (64-byte
   SHA hex + 8-byte size + 16 bytes of alignment / atime / mtime). At
   1 M blobs: ~80 MiB. Doesn't bound — a multi-million-blob store can
   exhaust the runtime's memory cap on a single `iter` call. Mitigate
   by capping store size via `cleanup` policy, or by switching ops
   scans to direct `opendir` walks of the shard tree.

5. **`shard_depth = 2` with sparse store wastes opendir syscalls.**
   `iter` walks 256×256 = 65 K possible shard directories. With most
   shards empty (e.g. < 1000 blobs total), iter does ~65 K syscalls
   for a few entries. Acceptable for ops scans; if it becomes a
   hot-path concern, add a "known shards" sidecar.

6. **Path length: caller must keep `app_dir + dir + shard + hash`
   under `PATH_MAX`** (1024 on macOS, 4096 on Linux/Cosmo). Worst case
   for `shard_depth = 2`: `app_dir/dir/blobs/XX/YY/<64-hex>` =
   `app_dir + dir + 1 + 6 + 3 + 3 + 64 = app_dir + dir + 77`. With
   `app_dir + dir` up to ~947 chars, fits on macOS. Hull's init
   checks via `snprintf` return so overruns become a clean `-1` at
   `blob.init()`, but the error message is generic ("init failed").
   In practice, app_dir is rooted under cwd which is rarely deeper
   than 100 chars; the limit only bites when users explicitly stash
   blobs under a deeply-nested system path (e.g.
   `/Library/Application Support/.../some/very/deep/cache/dir/`).
   Easiest mitigation: keep `dir` short (`"blobs"`, `"cache"`) and
   let app_dir absorb the depth.

## Performance baseline

Measured via `make bench-blob` on Apple M-series, APFS, default
allocator (no memory limit). Reproducible from `bench/blob/bench_blob.c`.
Numbers below reflect the post-§1.5.b-3.5+follow-up state: ARMv8 SHA2
hardware acceleration, `put_verified` short-circuit, durable opt-in,
`futimes`-on-fd atime bump.

| Workload | Throughput | Per-op cost |
|---|---|---|
| Put 4 KiB blob (buffer) | 5.5 K ops/s · 21 MB/s | 182 µs/op |
| Get 4 KiB blob (no atime) | 45 K ops/s · 176 MB/s | 22 µs/op |
| Get 4 KiB blob (atime on) | 24 K ops/s · 93 MB/s | 42 µs/op |
| Put 64 KiB (buffer) | 2.9 K ops/s · 181 MB/s | 346 µs/op |
| Put 64 KiB (stream, 16 chunks) | 2.6 K ops/s · 161 MB/s | 389 µs/op |
| Get 64 KiB (buffer, hot) | 38 K ops/s · 2.4 GB/s | 26 µs/op |
| **Idempotent put 64 KiB (no expected)** | **5.1 K ops/s · 320 MB/s** | **196 µs/op** |
| **`put_verified` short-circuit 64 KiB** | **518 K ops/s · 32 GB/s** | **1.9 µs/op** |
| **Durable put 64 KiB (fsync fd+dir)** | **2.6 K ops/s · 161 MB/s** | **388 µs/op** |
| Put 4 MiB (buffer) | 93 ops/s · 370 MB/s | 11 ms/op |
| Get 4 MiB (buffer, hot) | 3.1 K ops/s · 12 GB/s | 320 µs/op |
| Iter 10 K blobs | 260 K entries/s | 3.9 µs/entry |
| Iter 100 K blobs | ~300 K entries/s | 3.3 µs/entry |
| **SHA-256 raw (4 MiB)** | **2.0 GB/s** | (ARMv8 FEAT_SHA2) |

### Observations

- **`put_verified` short-circuit is ~270× faster than the idempotent
  put path.** When the caller supplies `expected` AND the blob already
  exists on disk, blob skips the tmp+hash+write entirely (two syscalls:
  validate_id + stat). 1.9 µs/op vs 196 µs/op. **Use this aggressively
  for verified-install workloads** (`hull tools install`, AOT cache
  lookups, signed-manifest fetches) — the metadata layer derives the
  expected SHA from the manifest, blob's check becomes a one-stat fast
  path on hit.

- **Hardware SHA-256 acceleration delivers ~10× speedup over portable
  software SHA-256.** Raw SHA throughput went from ~200 MB/s
  (sha256_transform_portable) to ~2 GB/s (sha256_transform_armv8) on
  M-series. Compounds with the put pipeline: large-blob put almost
  tripled (140 → 370 MB/s), and 64 KiB put nearly doubled (98 → 181
  MB/s). Three platform paths, runtime-dispatched on first transform
  call (one branch on a cached `int`):
    * **ARMv8-A FEAT_SHA2** — always-on for `__APPLE__` (Apple
      Silicon guarantees it on every shipped chip), runtime-detected
      via `getauxval(AT_HWCAP) & HWCAP_SHA2` for Linux/Cosmo arm64.
    * **x86_64 SHA Extensions** (Goldmont Plus / Skylake-X /
      Tiger Lake / Alder Lake / Zen+ and later) — runtime-detected
      via `CPUID.7.0:EBX[29]`.
    * **Portable C** — fallback for everything else (32-bit ARM,
      RISC-V, older x86 without SHA-NI).

- **Durable put costs ~12% throughput** on APFS (2.9K → 2.6K ops/s for
  64 KiB blobs, with `fsync(fd)` before close + `fsync(dirfd)` after
  rename). The cost is dominated by fsync(dirfd) — APFS journals
  directory entries quickly. Use the durable variant for content that
  can't be cheaply re-derived (user-uploaded files via attachment,
  release-artifact local cache); keep the default non-durable path
  for re-derivable caches (compute AOT, Lua bytecode, template AST,
  LLM artifacts).

- **Get is fast** when atime tracking is off; the `futimes(2)` syscall
  is the main overhead (~20 µs per read). **Always pass
  `{ track_access = false }` on hot read paths** like Lua bytecode
  lookup. The LRU policy can rely on filesystem `mtime` for those
  blobs (FIFO) or write a periodic atime-bump from a background timer.

- **Streaming vs buffer put is within 5%.** The chunk-loop overhead is
  trivial compared to SHA + I/O. Choose stream mode for memory-safety
  (don't buffer multi-MB uploads), buffer mode for code simplicity.

- **Iter scales sub-linearly per entry** at higher N (3.3 µs/entry at
  100 K, 4 µs/entry at 10 K) — directory readdir amortizes well over
  larger shards. Below ~1000 blobs the per-entry cost rises (~12–30
  µs) because the readdir overhead dominates the per-entry cost.

## Architectural notes (orthogonality + coupling)

- **Orthogonal API axes**: lifecycle (init/free), write (put/writer),
  read (get/reader), metadata (exists/size/atime/delete), enumeration
  (iter/count/total_size), eviction (cleanup). Each axis is
  independently testable.
- **Convenience-over-orthogonality**: `put` and `get` are convenience
  wrappers over `writer` / `reader`. `count` and `total_size` re-walk
  the shard tree each call; for hot accounting, cache results above
  blob or call once and snapshot.
- **Sandbox coupling is intentional**: the v0.1.9 sandbox change
  (`src/hull/sandbox.c`) pre-mkdirs declared `fs.write` paths so
  `realpath()` can canonicalize them for the seatbelt subpath rule.
  Driven by blob's lazy-init pattern but useful for any module that
  creates its own root directory at first use.
- **Refcount lives at the consumer**: blob doesn't know that
  `hull/web/attachment@1` will track refcounts in
  `_hull_attachments`. Cleanup driven from blob (LRU eviction)
  won't coordinate with consumer refcounts — never mix
  `blob.cleanup` with refcounted consumers on the same root.
  Use separate blob roots if you need both policies.

## Runtime-infrastructure caches and the manifest line

Several v0.1.10 migration consumers (Lua bytecode cache, compute AOT
cache, template AST cache, `hull tools install`) use blob as their
backing store but DO NOT appear in user apps' `app.manifest.fs.write`.
The reasoning:

| Path | Goes in manifest? | Why |
|---|---|---|
| `data/uploads/` (app's storage for user files) | **Yes** | App-chosen, app-visible, app-managed |
| `data/blobs/` (app-managed blob root for `hull/web/attachment@1`) | **Yes** | Same — app declares the dir |
| `~/.hull/blobs/runtime/lua-bytecode/` | **No** | Runtime infrastructure; app didn't ask for the cache |
| `~/.hull/blobs/runtime/compute-aot/` | **No** | Build-tool artifact cache; runs from `hull build`, not at app runtime |
| `~/.hull/blobs/runtime/templates/` | **No** | Runtime infrastructure |
| `~/.hull/blobs/tools/` | **No** | `hull tools install` runs from the shell, not from inside an app |

The line is **what the app deliberately produces or consumes vs. what
the runtime decides to cache to make the app faster**. The latter is
infrastructure, like a JIT cache in a language runtime or a system
shader cache in a graphics driver — not part of the app's capability
surface.

Precedent: Hull's sandbox already auto-allows infrastructure paths
the app never declared (embedded CA bundle when found, SQLite WAL/SHM
siblings, `~/.hull/tools/` resolution path). Runtime caches join that
list — see `src/hull/sandbox.c::wire_caps`.

Worst-case attack surface: a compromised app filling
`~/.hull/cache/` with junk → cache miss + re-derive on next use →
self-healing. The filename IS the SHA, so stale or wrong entries can't
be served (any read would fail validation OR the caller would
re-compute and overwrite). Bytecode/AOT/template caches are
deterministic content-addressed storage; a corrupted entry can't
trick the runtime into executing unintended code.

### What disclosure DOES exist

To preserve Hull's "code's visible permissions" property even when
runtime caches live outside the manifest:

- **`hull doctor`** reports cache locations and their sizes (similar
  to how it reports the CA bundle and tools status today).
- **`hull inspect`** surfaces "this binary uses caches at:
  `~/.hull/cache/...`" — informational, not a permission.
- **Opt-out env vars** — checked on every cache call (not memoized
  at process start), so flipping mid-process takes effect on the
  next access:

  | Variable | Effect when truthy |
  |---|---|
  | `HULL_NO_CACHE` | Disables every runtime cache (lua-bytecode, js-bytecode, compute-aot, templates). Tools store unaffected. |
  | `HULL_NO_LUA_BYTECODE_CACHE` | Lua bytecode cache only. |
  | `HULL_NO_JS_BYTECODE_CACHE` | QuickJS bytecode cache only. |
  | `HULL_NO_AOT_CACHE` | Compute AOT cache only. |
  | `HULL_NO_TEMPLATE_CACHE` | Template render-fn cache only. |

  Truthy = anything not in {empty, `0`, `false`, `FALSE`, leading
  `f`/`F`}. Disabling forces re-derive on every load.

- **`hull cache list|prune|clear`** surfaces what's actually
  stored, evicts old entries, or wipes everything. The Status
  column reflects per-cache opt-out state (`ok` / `off (env)` /
  `off (all)` / `n/a` for system stores) and a footer line names
  the active env var. JSON output includes `env_var` + `disabled`
  fields per kind.

- **Registry-driven**: `include/hull/cache_registry.h` is the
  single source of truth for cache kinds (display name +
  description + runtime/system flag + env_kind). Adding a new
  cache kind = one entry in `REGISTRY[]` and the new kind
  automatically gets:
    - matching `HULL_NO_<KIND>_CACHE` env var (composed from
      `env_kind`),
    - Status column in `hull cache list`,
    - a row in `hull doctor` Caches section,
    - a row in `hull inspect` runtime-caches disclosure,
    - participation in `hull cache prune` (if runtime) and
      `hull cache clear`.

  Zero per-surface code beyond the registry row.

### Per-app cache isolation (`HULL_CACHE_DIR`)

The default shared pool at `~/.hull/blobs/runtime/` is the right
tradeoff for development workstations (cross-app dedup of stdlib
bytecode is real). It's the wrong tradeoff on multi-tenant boxes,
CI runners with overlapping jobs, or systemd / k8s / Docker
deployments where one compromised process shouldn't be able to
poison another deployment's cache entries.

Setting `HULL_CACHE_DIR=/absolute/path` redirects the entire
runtime cache pool to that directory. Each deployment supplies
its own value; no two apps share a cache root:

```sh
# systemd unit
[Service]
Environment=HULL_CACHE_DIR=/var/lib/myapp/hull-cache

# k8s pod spec
env:
  - name: HULL_CACHE_DIR
    value: /run/cache/hull

# Docker
docker run -e HULL_CACHE_DIR=/cache -v cache_vol:/cache myapp
```

Rules:

- **Must be absolute.** Relative paths are rejected — keeps the
  resolved location obvious to the sandbox.
- **The sandbox auto-allows it.** No manifest changes required.
- **Opt-outs still apply.** `HULL_NO_CACHE=1` etc. work regardless
  of where the cache root resolves.
- **Tools storage is NOT redirected.** `~/.hull/blobs/tools/`
  stays at its stable system home — those are signed durable
  downloads, not per-app caches, and `hull tools install` runs
  from the shell at a different time than the apps themselves.
- **`hull cache list` shows the override active** so users can
  see when their per-app cache is in effect.

A planned later iteration (Layer C, optional) adds automatic
per-app isolation derived from app identity (`HULL_CACHE_PER_APP=1`
→ derive subdir from app signature). Today's manual override
covers the security-critical deployments; auto-isolation is a
convenience for paranoid defaults.

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
