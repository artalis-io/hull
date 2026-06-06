<!-- minimal -->
## Blob storage (`hull/blob@1`)

Pure content-addressed storage. Bytes in → SHA-256 hex id; bytes out by
id. Other modules layer policy (naming, refcounts, MIME validation,
auth, eviction) on top.

```lua
-- Lua
local blob = require("hull.blob")

app.manifest({
    modules = { "hull/blob@1" },
    fs      = { write = { "data/blobs/" } },
})

app.main(function()
    blob.init({ dir = "data/blobs" })

    local id, size = blob.put("hello, world")  -- id = sha256 hex
    local got      = blob.get(id)               -- "hello, world"

    -- Streaming for large blobs (on-the-fly SHA, never buffered)
    local w = blob.writer()
    w:write(chunk1):write(chunk2)
    local id2, size2 = w:finalize()
end)
```

```javascript
// JS
import { blob } from "hull:blob";

app.manifest({
    modules: ["hull/blob@1"],
    fs: { write: ["data/blobs/"] },
});

app.main(() => {
    blob.init({ dir: "data/blobs" });

    const { id, size } = blob.put("hello, world");
    const bytes = blob.get(id);                  // ArrayBuffer

    const w = blob.writer();
    w.write(chunk1).write(chunk2);
    const r = w.finalize();                      // { id, size }
});
```

Two callers putting identical bytes share storage automatically
(content-addressed dedup is free). No SQLite dependency — works under
`HL_ENABLE_DB=0` (compute-only builds).

<!-- compact -->
## Lifecycle

`blob.init({ dir, shard_depth?, tmp_max_age? })` — required before any
other call. Validates `dir` against `manifest.fs.write`, creates
`<dir>/blobs/` + `<dir>/tmp/` if absent, sweeps stale `.blob-*.tmp`
files older than `tmp_max_age` (default 3600s).

| Option | Default | Notes |
|---|---|---|
| `dir` | (required) | Relative to `app_dir`; must be covered by `fs.write` |
| `shard_depth` / `shardDepth` | 1 | 1-level (256 subdirs, ~25 M blob ceiling) or 2-level (~6 B ceiling) |
| `tmp_max_age` / `tmpMaxAge` | 3600 | Stale-tmp sweep threshold in seconds. 0 = default. |

Re-init is allowed (re-points to a new directory; previous handle is
freed via GC).

## Storage layout

```
<root>/
├── blobs/
│   ├── 00/    (or 00/00/ at shard_depth=2)
│   │   └── <hash>          file contents == sha256-hex
│   └── ...
└── tmp/
    └── .blob-<random>.tmp  in-flight writes
```

- **Filename IS the SHA-256 hex** — self-verifying: `sha256(file) ==
  basename(file)` always.
- Atomic writes: per-write tmp file + `rename(2)`. Readers never see
  partial bytes.
- Idempotent put: two concurrent puts of identical bytes both succeed;
  no locking needed.

## API reference

### Writes — on-the-fly SHA

```lua
-- Buffer-mode: one-shot
local id, size = blob.put(bytes)                          -- bytes = string
local id, size = blob.put_verified(bytes, expected_id)    -- raises on mismatch

-- Streaming: feed bytes through the hasher in lockstep with the
-- temp-file write — never buffered just to hash.
local w = blob.writer()                          -- or { expected = sha }
w:write(chunk1)
w:write(chunk2)
local id, size = w:finalize()                    -- atomic rename
-- OR
w:abort()                                        -- discard tmp

-- writer GC fires silent abort if neither finalize nor abort was called
-- (e.g. handler errored out) so tmp files don't leak.
```

```javascript
const { id, size } = blob.put(bytes);                     // string or ArrayBuffer
const r = blob.putVerified(bytes, expectedId);            // throws on mismatch

const w = blob.writer();                                  // or { expected: sha }
w.write(chunk1).write(chunk2);                            // chainable
const r = w.finalize();                                   // { id, size }
```

### Reads

```lua
local bytes = blob.get(id)                       -- nil if missing
local bytes = blob.get(id, { track_access = false })  -- skip utimes() on hot paths

local r = blob.reader(id)                        -- streaming reader
while true do
    local chunk = r:read(65536)
    if not chunk then break end
end
r:close()
```

```javascript
const buf = blob.get(id, { trackAccess: false }); // ArrayBuffer or null

const r = blob.reader(id);
let chunk;
while ((chunk = r.read(65536)) !== null) { /* … */ }
r.close();
```

### Metadata

```
blob.exists(id)        → bool
blob.size(id)          → int or nil
blob.atime(id)         → int (unix sec) or nil
blob.delete(id)        → bool (true if removed)
```

### Enumeration (snapshot)

```lua
for id, size in blob.iter() do … end
blob.count()           -- int
blob.total_size()      -- int (bytes)
```

```javascript
for (const { id, size } of blob.iter()) { … }
blob.count();  blob.totalSize();
```

Iter takes a snapshot at call time — adds/deletes during iteration
aren't reflected. Safe under concurrent put/delete.

### Opt-in eviction (never automatic)

```lua
local stats = blob.cleanup({
    max_total_size = 10 * 1024^3,   -- 10 GiB cap
    max_age        = 30 * 86400,    -- 30 days
    strategy       = "lru",         -- or "fifo"
    dry_run        = false,
})
-- stats = { removed = N, freed_bytes = B }
```

```javascript
const stats = blob.cleanup({
    maxTotalSize: 10 * 1024 ** 3,
    maxAge: 30 * 86400,
    strategy: "lru",   // or "fifo"
    dryRun: false,
});
// stats = { removed, freedBytes }
```

Some callers (attachment storage) never want eviction; others (LLM
artifact cache, compute AOT cache) want LRU. Cleanup is always
opt-in — never automatic.

<!-- full -->
## Layered patterns

### Verified install (signed-manifest workflow)

When the caller already knows the SHA-256 of the bytes (downloaded a
release artifact whose hash was signed in a manifest), pass it as
`expected` so the write fails fast on corruption:

```lua
local body = http.get("https://github.com/.../hull-darwin-arm64")
local sha = manifest.entries["hull-darwin-arm64"]   -- from signed manifest
local id, size = blob.put_verified(body, sha)
-- raises if body doesn't hash to `sha` — caller knows manifest was
-- authentic, so a mismatch means transport corruption or MITM.
```

### Hash-and-stash with metadata sidecar (no SQLite)

For build flavors without `HL_ENABLE_DB`, sidecar metadata via JSON:

```lua
local json = require("hull.json")
local fs   = require("hull.fs")

-- Read existing index (if any)
local index = {}
local existing = fs.exists("data/blobs.index.json") and
                 json.decode(fs.read("data/blobs.index.json")) or {}

-- Store and record
local id, size = blob.put(bytes)
existing[name] = { id = id, size = size, uploaded_at = time.now() }
fs.write("data/blobs.index.json", json.encode(existing))
```

This is the pattern `hull tools install` and the compute AOT cache
will use after their planned migrations to blob.

### LLM artifact cache

Caller-derived key:

```lua
local prompt_sha = crypto.sha256(prompt .. context .. model_id)

-- Cache hit?
local cached = blob.get(prompt_sha, { track_access = true })
if cached then return cached end

-- Miss: compute and store under the same key
local response = call_llm(prompt, context)
blob.put_verified(response, prompt_sha)   -- key IS the SHA
return response
```

Then evict periodically:

```lua
app.daily("03:00", function()
    blob.cleanup({ max_age = 7 * 86400, strategy = "lru" })
end)
```

### Hot-path read (avoid utimes syscall)

When reading stdlib bytecode (or any path you don't need atime tracking
on), opt out per call:

```lua
local code = blob.get(bytecode_sha, { track_access = false })
load(code)()
```

Default ON keeps LRU policy honest for the cases that need it; opt-out
skips the syscall.

## Sandbox interaction

`blob.init({dir})` validates `dir` via `hl_cap_fs_validate()` against
`manifest.fs.write`. The macOS seatbelt sandbox pre-creates declared
write directories at apply time (so realpath can canonicalize them),
which means blob's first `mkdir_p` of the root works under the
sandbox without any hand-mkdir from user code.

## Non-goal: encryption at rest

Blob does not encrypt stored bytes — encryption-at-rest breaks
content-addressing (random-nonce loses dedup; convergent encryption
introduces confirmation-of-file attacks). Encrypt at the consumer
layer instead:

```
1. Caller hashes plaintext → metadata.plaintext_sha (dedup key)
2. Caller encrypts plaintext → ciphertext
3. blob.put(ciphertext) → blob_id (ciphertext SHA)
4. Metadata: { id, plaintext_sha, blob_id, … }
```

See `docs/blob.md §Non-goal: encryption at rest` for the full
rationale and the recommended consumer-side pattern.

## Build-flavor compatibility

| Flag | Effect on blob |
|---|---|
| `HL_ENABLE_DB=0` | No effect — zero SQLite dependency |
| `HL_ENABLE_HTTP_SERVER=0` | No effect — callable from `app.main()` |
| `HL_ENABLE_HTTP_CLIENT=0` | No effect — no network |
| `HL_ENABLE_WASM=0` | No effect |

A compute-only build still gets the full blob primitive — which is
exactly where the compute AOT cache and Lua bytecode cache live.

## See also

- `docs/blob.md` — full design (storage layout, invariants, migration
  map for tools/compute AOT/template caches)
- `hull/web/attachment@1` (planned, §1.5.b-4) — web upload + serve
  layer built on top of blob
- `hull/crypto@1` (`crypto.create_sha256` / `createSha256`) — the
  incremental hasher blob uses internally
