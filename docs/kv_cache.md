# KV / cache subsystem (`hull.kv` + `hull.cache`)

Two small, portable abstractions over local, SQL-backed, and (future)
distributed key/value engines. The design goal is a clean semantic seam, **not**
a Redis clone: define the minimal portable operation set + a capability model,
and let mature storage engines do the storage.

## `cache` is NOT a durable KV store

This distinction is load-bearing. The two modules exist because the guarantees
differ, and code that confuses them will lose data or leak memory.

| | **`hull.cache`** (CACHE) | **`hull.kv`** (KV STORE) |
|---|---|---|
| Lifetime | ephemeral | externally meaningful |
| Eviction | **expected** (LRU on cap) | **never**, unless you ask (`cleanup`) |
| Values | recomputable | authoritative |
| Bounded | yes (`max_bytes` / `max_items`) | unbounded by default |
| TTL | common | optional |
| Persistence | no (SQL cache is still evicting) | where the backend provides it |
| Optimized for | fast local reuse | correctness + durability |

If losing an entry is a correctness bug, it is KV state, not a cache.

## Keys and values are bytes

Keys and values are arbitrary bytes: Lua strings (natively binary-safe) and, in
JS, **byte strings** (each char a code unit 0-255 - Hull's JS byte convention,
the same one `crypto/_hex` and `jwt` use). `.length` / `#v` is the byte count;
there is no UTF-8 assumption. Text values are stored as their bytes; for large
binary blobs prefer `hull/blob`.

- `get` on a **miss returns `nil` / `null`** (absence is a value, per the stdlib
  error convention). A backend/transport error **throws** a coded error.
- Coded errors carry a stable `.code`: `invalid_argument`, `unsupported`,
  `capacity_exceeded`, `conflict`.
- Max key size is 1024 bytes; values are bounded only by the backend (and, for a
  memory cache, the byte budget).

## API

```lua
local kv = require("hull.kv").open{ backend = "sqlite", database = db,
                                    namespace = "agent-state" }
kv:set("run:123", payload)          -- bytes -> bytes
local v = kv:get("run:123")         -- bytes | nil
kv:set(k, v, { ttl = 3600 })        -- TTL in seconds (capability-gated)
kv:delete(k); kv:exists(k)
kv:incr("hits", 1)                  -- atomic increment (capability-gated)
kv:cas(k, expected, new)            -- compare-and-swap; expected=nil => set-if-absent
kv:scan("run:")                     -- prefix iteration -> { keys } (capability-gated)
kv:clear()                          -- namespace-scoped wipe
kv:cleanup()                        -- delete expired rows (SQL); returns count
kv.caps                             -- { ttl, atomic_increment, compare_exchange,
                                    --   scan, persistent, shared, eviction, transactions }

local cache = require("hull.cache").open{ backend = "memory", namespace = "query-ir",
    max_bytes = 512*1024*1024, max_items = 100000, default_ttl = 600 }
cache:set(k, bytes); cache:get(k)
cache:fetch(k, 60, function() return render() end)   -- get-or-compute (bytes)
cache:stats()                       -- { hits, misses, evictions, items, bytes }
```

JS mirror (sync; `import { kv } from "hull:kv"`, `import { cache } from "hull:cache"`)
with camelCase options (`maxBytes`, `defaultTtl`, `maxItems`) and `store.get(k)`
returning `null` on a miss.

`hull.cache.open{}` is **additive**: the shipped top-level `cache.get/set/fetch/new`
(a lightweight in-process memoizer for arbitrary values) is unchanged. Reach for
`cache.new()` for a quick value memoizer, `cache.open{}` when you need byte
accounting, a backend, or explicit namespaces.

## Capability model

Backends do not pretend to uniform guarantees. Each advertises a `caps` set;
an optional op on a backend that lacks the cap **throws `unsupported`** rather
than silently no-opping. Verified against the implementation:

| capability | memory | sqlite | postgres | valkey/redis | cachelib¹ |
|---|:---:|:---:|:---:|:---:|:---:|
| local / in-process | ✅ | ✅ | ❌ | ❌ | ✅ |
| persistent | ❌ | ✅ | ✅ | ✅ | limited (SSD) |
| shared (cross-process) | ❌ | ❌² | ✅ | ✅ | ❌ |
| TTL | ✅ | ✅ | ✅ | ✅ | ✅ |
| eviction (cache policy) | ✅ LRU | ✅ SQL | ✅ SQL | ✅ native | ✅ |
| atomic increment | ✅ | ✅ | ✅ | ✅ | ❌ |
| compare-and-swap | ✅ | ✅ | ✅ | ✅ | ❌ |
| scan / prefix | ✅ | ✅ | ✅ | ✅ | limited |
| transactions | ❌ | ✅ | ✅ | backend | ❌ |

¹ planned backend (see extension points); valkey/redis is SHIPPED
(`--with=valkey`). ² a SQLite file can be opened by
multiple processes but is a local file, not a shared service; `caps.shared` is
`false`. TTL is app-managed (a lazily-filtered `expires_at`) on every SQL
backend - the same capability regardless of how it is implemented.

## SQL-backed KV

The SQL backend reuses Hull's backend-agnostic `db` capability - **no second
connection stack**. You pass an existing connection (`database = db`); every
write goes through the same parameterized `query`/`exec`/`ON CONFLICT` the rest
of the stdlib uses, so SQLite and PostgreSQL are served by one implementation.

One table, backend-portable (all `TEXT`/`BIGINT`, protected by the `_hull_`
prefix so app code cannot touch it):

```sql
CREATE TABLE _hull_kv (
    ns TEXT NOT NULL, k TEXT NOT NULL, v TEXT NOT NULL,
    expires_at BIGINT NOT NULL, version BIGINT NOT NULL DEFAULT 1,
    created_at BIGINT NOT NULL, updated_at BIGINT NOT NULL,
    PRIMARY KEY (ns, k)
);
```

- Keys are **hex-encoded** (prefix-preserving, so `scan` is a `LIKE` range) and
  values **base64-encoded**, both into portable `TEXT` - Postgres `TEXT` rejects
  embedded NULs, so raw binary cannot be stored directly. This makes durable KV
  byte-safe **and cross-runtime** (a value written by Lua reads back identically
  under JS).
- `put` is a native `INSERT ... ON CONFLICT (ns,k) DO UPDATE` (SQLite +
  Postgres); `cas` is a single conditional `UPDATE ... WHERE v = ?` (or an
  `INSERT ... ON CONFLICT DO NOTHING` for set-if-absent) - genuinely atomic, not
  a read-then-write. `incr` is an optimistic version-guarded retry loop.
- Cache-over-SQL eviction (`max_items`) is expressed as SQL
  (`DELETE ... ORDER BY updated_at LIMIT`), never in C.
- TTL is a lazily-filtered `expires_at` (`WHERE expires_at > now`); `cleanup()`
  deletes expired rows.

## Security

The KV layer opens **no hidden fs or network access**. A SQLite-backed store
runs through the `db` capability, so the file honors the fs sandbox exactly like
`fs.read`/`db.open`; a network backend (Postgres, or Valkey/Redis via the
`kv.dynamic` allowlist) honors the host allowlist and the `network_outbound`
sandbox grant. Namespaces
are first-class and isolate keyspaces; a `kv` and a `cache` that share a
namespace name never collide (physical namespaces are prefixed `kv:` / `cache:`).

**Namespaces are process-lifetime.** The memory backend keys one store per
namespace at module level (so same-namespace opens share state), and that map is
never pruned. Keep the set of namespaces **bounded** - do not derive a namespace
from unbounded / attacker-controlled input (e.g. `namespace = "tenant:" .. id`
for an unbounded id set), or each distinct value leaks a store for the process
lifetime. A durable (SQL) namespace is just an `ns` column value, so it does not
leak process memory, but the same bounded-cardinality guidance applies.

## Backend extension points

The subsystem is deliberately narrow; new engines slot in without touching the
semantic layer.

- **Native C cache store** (fast-follow): a small `cap/` module (hashmap +
  intrusive LRU + byte accounting) implementing the same store interface the
  Lua/JS memory backend presents, for high-throughput local caches. The stdlib
  memory backend already byte-accounts, so this is a performance drop-in, not a
  correctness prerequisite.
- **Valkey / Redis** (shipped: the preferred distributed backend): the
  `--with=valkey` composable feature. Redis is a command protocol, not a query
  language, so it does NOT go behind `HlDbBackend` / `hl_db_feature_backends`;
  it fills a NEW, narrow non-SQL seam - the `HlKvBackend` vtable
  (`include/hull/cap/kv_backend.h`) and its own weak hook
  `hl_kv_feature_backends` (`cap/kv_feature.c`), a sibling of
  `hl_db_feature_backends` so the two compose side by side. The feature is
  reached by a `valkey://` / `redis://` (or `rediss://` / `valkeys://`) DSN on
  `kv.open` / `cache.open` (`backend = "valkey"`), gated by the manifest
  `kv.dynamic` allowlist and the same network sandbox grant as a network DB. It
  packages exactly like the Postgres/MySQL wire backends (pure C, no vendored
  engine, RESP2/RESP3, ACL/password auth, `rediss://` TLS via the shared TLS
  client). The portable KV subset only; there is NO generic Redis-command escape
  hatch, and cluster / Sentinel / streams / pub-sub / sorted-sets / scripting are
  out of scope and belong in separate future capabilities.
- **CacheLib**: evaluated and **declined** - see the design spike
  [docs/cachelib_spike.md](cachelib_spike.md). Meta's CacheLib pulls the full
  folly/fbthrift/fizz/wangle stack (~15-20 C++ deps, OpenSSL/boost), is Linux-only
  (no macOS/cosmo), has no C ABI, and owns its own allocator + background threads
  + SSD I/O - incompatible with Hull's vendored-only, four-platform, `HlAllocator`
  + sealed-arena model. The in-model answer for a fast local cache is the native
  C cache store (the `cap/kvmem.c` follow-up above), not CacheLib.
- **DuckDB**: usable as a KV backend where its semantics fit; the code does not
  force transactional guarantees DuckDB does not naturally provide.

## Tests

`tests/e2e_kv.sh` (`make e2e-kv`): a conformance vector run against **both** the
memory and sqlite backends in one process (the app asserts they match), then
compared across Lua and JS; durability across a process restart; and
`cache.open` LRU + byte-budget eviction. `tests/e2e_cache_module.sh` continues
to cover the legacy `cache.new` value memoizer.
