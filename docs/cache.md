# Hull Cache Layer

This document is the canonical reference for Hull's on-disk caches: what
they are, where they live, how to inspect them, how to discipline their
size, and how to isolate them between applications. It is the place a
new operator should land when they want to understand "why is there a
`.hull/` directory on this machine".

For the low-level CAS primitive shared across every cache, see
[docs/blob.md](blob.md). For the WASM AOT story specifically see
[docs/wamr_architecture.md](wamr_architecture.md). For the signed-tool
trust chain that backs the `tools` store, see
[docs/tools_install.md](tools_install.md).

## Why a cache layer

Hull recompiles a lot. On every cold start a single app can run:

- `luaL_loadbuffer` on every stdlib + app `.lua` file
- `JS_Eval(COMPILE_ONLY)` on every stdlib + app `.js` module
- Template parse + Lua/JS codegen for every `templates/*.html`
- `wamrc` AOT compilation for every `compute/*.wasm` (if `wamrc` is
  installed and an `.aot.<arch>` isn't already next to the `.wasm`)

Cold-start cost adds up. Hull caches the *output* of every one of those
compilations on disk, keyed by a hash that captures the inputs (source
bytes, compiler version, runtime version, architecture). On warm starts
each compile is replaced by a single `mmap` + memcpy, and the runtime
boots in milliseconds.

The cache is opportunistic and disposable: every entry is re-derivable
from source code in the application itself. You can `rm -rf ~/.hull/`
at any time — the next request rebuilds whatever's missing. That
property is what makes the cache safe to share, share-isolate, prune,
or wipe without ceremony.

## Layout

Everything lives under `$HOME/.hull/blobs/`:

```
$HOME/.hull/blobs/
├── runtime/                  ← apps' runtime caches (this doc)
│   ├── lua-bytecode/
│   │   └── blobs/<XX>/<sha256>
│   ├── js-bytecode/
│   │   └── blobs/<XX>/<sha256>
│   ├── compute-aot/
│   │   └── blobs/<XX>/<sha256>
│   ├── templates/
│   │   └── blobs/<XX>/<sha256>
│   └── js-templates/
│       └── blobs/<XX>/<sha256>
└── tools/                    ← signed system store (NOT swept)
    └── blobs/<XX>/<sha256>
```

Each kind is one [`hl_blob_store`](blob.md) instance. Filenames are
sharded one level deep by the first two hex chars of the key (a flat
directory becomes pathological at ~50K entries — sharding keeps
`readdir` linear and ext4/HFS+ happy).

### What each kind caches

| Kind | What's stored | Cache key inputs | Hit path saves |
|------|---------------|------------------|----------------|
| `lua-bytecode` | `lua_dump()` output for every stdlib + app `.lua` | source sha256 + Lua version + `strip=0` flag | `luaL_loadbuffer` parse |
| `js-bytecode` | `JS_WriteObject(JS_WRITE_OBJ_BYTECODE)` for every stdlib + app `.js` | source sha256 + QuickJS version + module name | `JS_Eval` parse |
| `compute-aot` | `wamrc` AOT output for every `compute/*.wasm` | wasm sha256 + wamrc version + target triple | full `wamrc` invocation (seconds) |
| `templates` | Generated Lua render function (bytecode) for every `templates/*.html` | template source sha256 + engine version | template parse + Lua codegen + `luaL_loadbuffer` |
| `js-templates` | Generated JS render function (bytecode) for every `templates/*.html` | template source sha256 + engine version + name | template parse + JS codegen + `JS_Eval` |
| `tools` | Signed binaries downloaded by `hull tools install` (e.g. `wamrc`) | sha256 of the binary | full `~6 MB` HTTPS download + Ed25519 verify |

The five runtime caches share the same C primitive (`hl_blob_store_*`)
and the same management commands. `tools` is a separate "system store":
it's content-addressed in the strict sense (filename IS `sha256(contents)`)
because the install pipeline only writes a file once the sha matches the
signed manifest. The runtime caches use the same primitive in *keyed
mode* — the filename is a sha derived from inputs (source + version)
that doesn't equal `sha256(contents)`, because the contents are
compiler output that isn't known until after the key is computed.

## CLI

The complete surface is `hull cache <verb>` plus the auxiliary
`hull doctor` / `hull inspect` panels.

### `hull cache list [--json]`

Shows every registered cache with on-disk path, entry count, total
size, and a status column reflecting any active opt-out.

```
$ hull cache list
NAME          ENTRIES         SIZE  STATUS  PATH
lua-bytecode      213       1.3 MB  ok      /Users/me/.hull/blobs/runtime/lua-bytecode
js-bytecode         0          0 B  ok      /Users/me/.hull/blobs/runtime/js-bytecode
compute-aot         4       2.1 MB  ok      /Users/me/.hull/blobs/runtime/compute-aot
templates          11        38 KB  ok      /Users/me/.hull/blobs/runtime/templates
js-templates        0          0 B  ok      /Users/me/.hull/blobs/runtime/js-templates
tools               1       6.4 MB  n/a     /Users/me/.hull/blobs/tools

set HULL_NO_<KIND>_CACHE=1 to disable a single cache (e.g.
HULL_NO_LUA_BYTECODE_CACHE=1); HULL_NO_CACHE=1 to disable all
runtime caches. HULL_CACHE_DIR=/abs redirects runtime caches.
```

The `STATUS` column reads `ok` when caching is active, `off (env)` when
the per-kind opt-out is set, `off (all)` when `HULL_NO_CACHE` is set,
and `n/a` for `tools` (which is a signed download store, not an
opt-out-able cache).

### `hull cache prune [--kind=K] [--max-size=N] [--max-age=N] [--strategy=lru|fifo] [--dry-run] [--json]`

Evict entries by total size or by age. With no kind, sweeps every
runtime cache; `--kind=K` restricts to one. The `tools` store is
**not** swept by `prune` even without `--kind` — it's durable signed
content. Wipe it via `clear --kind=tools`.

- `--max-size=N` — accepts `K` / `M` / `G` (binary, 1024-based; trailing
  `B` optional). Evict oldest entries until the cache is ≤ N bytes.
- `--max-age=N` — accepts `s` / `m` / `h` / `d` / `w` / `y` (bare numbers
  treated as seconds for back-compat). Evict entries whose mtime is
  older than N.
- `--strategy=lru` (default) — evicts by least-recently-used (atime).
- `--strategy=fifo` — evicts by file mtime (when the entry was written).
- `--dry-run` — print what *would* be evicted without touching disk.
- `--json` — machine-readable; one result object per kind plus totals.

Example:
```
$ hull cache prune --max-size=100M --max-age=30d
lua-bytecode: removed 18 entries, freed 92 KB
templates:    removed 0 entries, freed 0 B
...
Total: removed 18 entries, freed 92 KB
```

### `hull cache clear [--kind=K] --yes [--json]`

Wipe runtime caches entirely. Requires `--yes`. With `--kind=K`,
restricts to one cache. `clear --kind=tools --yes` is the only way to
remove installed tools without going through `hull tools uninstall`.

### `hull cache verify [--kind=K] [--repair] [--json]`

Walk every entry and flag corruption. For CAS-mode kinds (`tools`),
recomputes `sha256(contents)` and compares to the filename — that's
the strongest possible integrity check the layer can do without
re-running the source compile. For keyed-mode kinds (runtime caches),
the filename doesn't equal `sha256(contents)`, so verify falls back to
a structural check (file exists, regular file, non-empty, readable).

```
$ hull cache verify
  lua-bytecode (keyed — structural only mode)
    213 checked, 213 ok, 0 corrupt
  ...
  tools (content-addressed — sha256 check)
    1 checked, 1 ok, 0 corrupt

Total: 218 checked, 218 ok, 0 corrupt
```

`--repair` unlinks any corrupt entries. The next compile / install
repopulates from source, so repair is safe even on a hot cache.
Without `--repair`, `verify` exits non-zero if anything is corrupt.

### `hull doctor` cache panel

`hull doctor` includes a `caches:` section that reports each kind's
size and warns when a single runtime kind exceeds **250 MB** or the
total runtime footprint exceeds **1 GB**. The hint points at the
right prune flag:

```
caches:
  lua-bytecode    213 entries   1.3 MB   ok
  compute-aot       4 entries   2.1 MB   ok
  templates        11 entries    38 KB   ok
  ...
  total runtime: 9 entries, 4.0 GB
  WARNING: total runtime cache is large; run
    hull cache prune --max-size=500M
  to bring it under control.
```

### `hull inspect` cache panel

`hull inspect` includes the same listing as `cache list`, plus
explicit notes about the active `HULL_CACHE_DIR` override and the
per-cache opt-out env vars. It's the right place to start when
debugging "why isn't my warm path warm".

## Environment variables

| Variable | Default | Effect |
|----------|---------|--------|
| `HULL_CACHE_DIR=/abs/path` | unset | Redirect **all runtime caches** to a custom absolute path. Used for per-app isolation; does not move `tools/`. |
| `HULL_NO_CACHE=1` | unset | Disable **every** runtime cache (kill switch). `tools` is unaffected. |
| `HULL_NO_LUA_BYTECODE_CACHE=1` | unset | Disable just the Lua bytecode cache. |
| `HULL_NO_JS_BYTECODE_CACHE=1` | unset | Disable just the QuickJS bytecode cache. |
| `HULL_NO_AOT_CACHE=1` | unset | Disable just the compute AOT cache. |
| `HULL_NO_TEMPLATE_CACHE=1` | unset | Disable just the Lua template cache. |
| `HULL_NO_JS_TEMPLATE_CACHE=1` | unset | Disable just the JS template cache. |

**Truthiness.** All `HULL_NO_*` variables follow the same rule: set to
any non-empty value other than the case-insensitive strings `0`, `false`,
`no`, or `off` to disable the cache. `=1`, `=true`, `=on`, `=yes`,
`=anything` all mean "off"; `=0` / `=false` / `=no` / `=off` / unset
mean "on (cache active)".

When a cache is disabled at runtime, the application **still works** —
it just pays the cold-start cost on every boot. There's no behavioural
divergence between "cache hit", "cache miss + write", and "cache
disabled"; the only observable difference is wall-clock time and disk
writes.

## Per-application isolation

By default every Hull process on a host shares one `~/.hull/blobs/runtime/`.
That's the right behaviour for a developer laptop: one cache, all your
apps benefit from it, easy to inspect.

On multi-tenant servers (one host running unrelated apps for different
customers, side-by-side k8s pods on the same node, CI jobs that touch
each other's bytecode) you usually want each app's cache scoped to that
app. Set `HULL_CACHE_DIR` to an absolute path:

```sh
export HULL_CACHE_DIR=/var/lib/myapp/cache
hull dev
```

When set, every runtime cache lives under `$HULL_CACHE_DIR/<kind>/`
instead of `$HOME/.hull/blobs/runtime/<kind>/`. The sandbox (Linux
unveil, macOS Seatbelt) auto-allows the override path so caching keeps
working under the kernel sandbox. The `tools/` store is **not**
redirected — it's a signed download cache that benefits from being
shared across all apps, and rotating it per-app would force every app
to re-download `wamrc` (and pay the verify cost) on first use.

`HULL_CACHE_DIR` composes with everything else: per-cache opt-outs
still work (`HULL_NO_LUA_BYTECODE_CACHE=1` disables Lua bytecode in
the overridden location), `cache list` reports the overridden paths,
`cache prune` operates on the overridden location, `cache verify`
verifies the overridden cache. The override path must be absolute;
relative paths are rejected with a clear error to avoid surprises
when `cwd` is something unexpected (e.g. inside a systemd unit).

### Deployment recipes

**systemd:**
```ini
[Service]
Environment="HULL_CACHE_DIR=/var/cache/myapp/hull"
ExecStart=/usr/local/bin/hull /var/lib/myapp/app.lua
StateDirectory=myapp/hull
```

**Docker:**
```dockerfile
ENV HULL_CACHE_DIR=/var/cache/hull
VOLUME /var/cache/hull
```
A volume mount means the cache survives container restarts; without
it the cache is rebuilt on every container start (still cheap, but
not free).

**Kubernetes:**
```yaml
env:
  - name: HULL_CACHE_DIR
    value: /var/cache/hull
volumeMounts:
  - name: cache
    mountPath: /var/cache/hull
volumes:
  - name: cache
    emptyDir: {}            # per-pod, dies with pod
    # or:
    # persistentVolumeClaim: { claimName: hull-cache }  # survives reschedule
```

`emptyDir` is right for stateless deployments — the cache is
rebuildable, and per-pod isolation keeps caches from leaking between
tenant pods on the same node. Use a PVC when cold-start latency
matters more than isolation.

## Concurrency

Multiple Hull processes can hammer the same cache simultaneously
without coordination. Writes go through the standard atomic pattern:
write to `<key>.tmp.<pid>.<rand>` in the same directory, then
`rename(2)` into place. POSIX guarantees the rename is atomic, so a
reader either sees the complete file or doesn't see it at all — never
a partial write.

The cache is verified concurrency-safe in CI by `tests/e2e_cache_concurrent.sh`,
which spawns 8 parallel workers writing to the same cache (and a
separate test mixing 4 Lua + 4 JS workers) and asserts no crashes, no
zero-sized blobs, no leftover `.tmp.*` files, and idempotent results
on warm replay.

The trade-off: two workers can produce two different writes for the
same key, with the later `rename` winning. That's harmless for the
runtime caches — every key derives from inputs that determinise the
output, so "two different writes" can only mean both wrote the same
bytes via different code paths.

## When the cache helps (and when it doesn't)

**Helps a lot:**
- Cold-start latency in containers / VMs that recycle frequently
- CI tasks that run the same app many times (test runners, agents)
- Apps with large stdlibs and many templates
- WASM compute apps with `wamrc` AOT enabled
- HTTP server boot under load balancers that recycle backends

**Doesn't matter:**
- Long-running servers that boot once and stay up — only the first
  request pays the parse cost
- Tiny apps with one entry point and no templates (cache overhead is
  ~negligible either way)
- Hot-reload development loops — the parse cost is sub-millisecond
  for an app with no `.lua` to load and `hull dev` reloads in tens of
  ms regardless

**Actively hurts (turn it off):**
- Read-only root filesystems where `~/.hull/` can't be written and
  the cache layer adds a per-request stat that always misses. Set
  `HULL_NO_CACHE=1` to skip the disk path entirely.
- Tiny ephemeral functions (FaaS) where the function body is a few
  lines of Lua and the cache lookup itself dominates parse time.
  Set `HULL_NO_CACHE=1`.

There's a 256-byte source floor: sources smaller than 256 bytes
**never** cache — the disk roundtrip is more expensive than the
parse. This is automatic; you don't need to opt in.

## What's NOT cached

For completeness, here's what Hull does *not* cache on disk:

- **Compiled regexes** — kept in-memory per VM, never persisted
- **HTTP responses** — that's an application concern (use `hull/middleware/etag`,
  ratelimit, Cache-Control headers)
- **Database query results** — that's SQLite's job (page cache, etc.)
- **`http.fetch` results** — set `Cache-Control` headers and use a
  reverse-proxy if you want HTTP caching
- **TLS handshakes** — mbedTLS owns this
- **WebGPU compiled shaders** — `gpu.compile()` cache is process-local
  (in-memory map). Compiled shader output is not currently persisted
  across runs. The compile cost is sub-millisecond per shader, so the
  on-disk cache hasn't been worth the bytes.
- **`compute.wasm` modules** — the `.wasm` file itself is embedded in
  the binary by `hull build`. Only the *AOT compilation* of those
  modules is cached, not the bytes themselves.

## Internals (one paragraph each)

**Storage primitive.** Every cache is an `HlBlobStore` (see
`include/hull/blob_store.h`). Writes go through `hl_blob_store_writer_*`,
which writes to a unique tmp path and renames into the sharded blob
directory. Reads go through `hl_blob_store_reader_*`, which `open`s
the file and (for the bytecode/template caches) `mmap`s it for
zero-copy parse. Iteration uses a snapshot of `readdir` results so
concurrent writes don't cause double-counting in `list` / `prune`.

**Cache registry.** All six kinds are listed in `src/hull/cache_registry.c`
as a static `HlCacheKind[]` array. Adding a new cache means one row in
that table — `list`, `prune`, `clear`, `verify`, `doctor`, and
`inspect` all iterate the registry, so a new cache automatically
appears in every CLI surface.

**Path resolution.** `hl_hull_cache_subdir(kind, out, sz)` returns the
on-disk path for a runtime cache. Honours `HULL_CACHE_DIR`. The
sandbox layer calls the same resolver to decide which path to allow,
so cache writes are never blocked by Seatbelt / unveil.

**Eviction.** LRU uses `st_atime` (last access time), FIFO uses
`st_mtime` (write time). Atime drift on Linux mounts mounted with
`noatime` makes LRU degrade to FIFO; this is fine in practice because
both metrics correlate with usefulness.

**Opt-out check.** `hl_hull_cache_disabled(kind)` returns nonzero when
either `HULL_NO_CACHE` or `HULL_NO_<KIND>_CACHE` is set to a truthy
value. The check is hoisted into a cached `int` per kind on first
call, so subsequent cache lookups pay one branch instead of one
`getenv` per access.

## Migration from earlier Hull versions

- **Pre-X-2:** Caches lived at scattered paths under `$HOME/.hull/`
  (e.g. `$HOME/.hull/cache/lua/`, `$HOME/.hull/aot/`). After the
  X-2 refactor all runtime caches moved under
  `$HOME/.hull/blobs/runtime/<kind>/`. The old paths are not read
  on upgrade — just leftover bytes; `rm -rf` the stale directories
  and let the new layout repopulate.
- **`HULL_NO_BYTECODE_CACHE`** was renamed to
  `HULL_NO_LUA_BYTECODE_CACHE` for symmetry with the JS opt-out.
  The old variable is silently ignored.
- **`hull cache verify` / `--repair`** were introduced after the
  X-2 refactor and only operate on the new layout.

## See also

- [docs/blob.md](blob.md) — low-level CAS primitive shared by every
  cache, plus the `hull/blob@1` application-facing module
- [docs/wamr_architecture.md](wamr_architecture.md) — WASM AOT
  compilation and the AOT cache lifecycle
- [docs/tools_install.md](tools_install.md) — the signed `tools`
  store
- [docs/security.md](security.md) — sandbox interaction with the
  cache directory
- [docs/roadmap_next.md](roadmap_next.md) — outstanding cache work
  (LLM response cache, Layer C per-app auto-isolation)
