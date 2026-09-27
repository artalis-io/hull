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
the same one `hull:encoding` uses). `.length` / `#v` is the byte count;
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

## Encryption at rest

`hull.kv.open` takes an optional `encrypt` keyring. Every value written through
the handle is then sealed with authenticated encryption (NaCl secretbox,
XSalsa20-Poly1305) before it reaches the backend, and checked when it is read
back. Someone who can read the backend (a database administrator, a backup,
another app on the same Postgres) sees noise; someone who can write it cannot
produce, alter or move a value without the key.

```lua
local env, crypto = require("hull.env"), require("hull.crypto")
local kv = require("hull.kv").open{
    backend = "postgres", database = db, namespace = "sessions",
    encrypt = { keys = { [1] = crypto.hex_decode(env.get("KV_KEY_1")) }, current = 1 },
}
```

```javascript
const store = kv.open({ backend: "postgres", database: db, namespace: "sessions",
                        encrypt: { keys: { 1: keyBytes }, current: 1 } });
```

- **Keys** are exactly 32 random bytes (`crypto.random(32)`), under integer ids
  0..2^32-1. `current` names the key new writes use. Keep them out of source:
  read them through the manifest's `env` allowlist. A malformed keyring fails
  `kv.open` with `invalid_argument`.
- **Binding.** The namespace and the key name are sealed inside each value, so
  a genuine value copied to another key or namespace does not open.
- **Failure** is one code, `decrypt_failed`, for every way a value can be
  wrong: altered, forged, moved, or sealed under a key id the keyring does not
  hold. It does not say which check failed.
- **Operations.** `get`, `set` and `cas` seal and open; `cas` compares the
  opened value with `expected` and swaps on the exact stored bytes, so it stays
  atomic. `incr` is `unsupported` on an encrypted handle (the backend cannot do
  arithmetic on ciphertext): keep a secret counter as a value and update it with
  `cas`. `delete`, `exists`, `scan`, `clear` and TTLs work on keys and are
  unchanged. `hull.cache` handles have no `encrypt` option.
- **Rotation.** Add the new key under a new id and make it `current`; values
  sealed under the old id still open, and each write re-seals under the new
  one. `kv:rekey(prefix?)` re-seals everything under the prefix that is not on
  the current key and returns how many it changed; it is safe to interrupt and
  rerun, and needs `scan` + compare-and-swap. **Drop the old key only after
  `rekey` has finished**, or the values it still covers stop opening.
- **Migrating a plaintext namespace.** Open it with `allow_plaintext = true`
  (`allowPlaintext` in JS): a value that does not open is returned as stored,
  on the assumption it predates encryption. Run `kv:rekey()`, then remove the
  flag. While the flag is set, a plaintext value **written by an attacker is
  accepted too**, so keep that window short.

What it does not protect against:

- **Deletion.** A writer can remove any entry.
- **Rollback.** A writer who kept an older genuine value for a key can put it
  back, and it still opens. Ruling that out needs a version counter kept
  outside the store.
- **Key names.** Only values are encrypted; key names stay visible (scan needs
  their prefixes). An app that must hide them hashes them itself.
- **Anyone who holds the key**, including the app's own process: keys are
  Lua / JS strings and live in the interpreter heap.

The format is `hull.crypto.sealbox` (`hull:crypto:sealbox`), shared with
TOTP's encrypted secrets; Lua and JS produce and accept the same bytes. Design
and threat model: [`kv_encryption_design.md`](kv_encryption_design.md).

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
  [docs/archive/design_records/cachelib_spike.md](archive/design_records/cachelib_spike.md). Meta's CacheLib pulls the full
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
