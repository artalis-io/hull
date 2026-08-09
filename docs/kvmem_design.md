# `cap/kvmem.c` native cache store — design spike (CONCLUDED: not shipped)

**Status:** CONCLUDED — experiment run, gates measured, **did not ship**. The
store was implemented on a throwaway branch (`feat/kvmem-native`, unpushed) and
measured against the five gates below. **Gate 2 (performance) and Gate 4
(resident memory) failed decisively and structurally**, so the native store was
NOT merged; the pure-Lua/JS `_memstore` remains the sole memory backend. Full
methodology, exact numbers, and root causes:
[kvmem_negative_result.md](kvmem_negative_result.md).

Why it failed, in one line: semantic correctness (Gate 1, passed) and an O(1)
eviction win (30-90x, the one bright spot) were NOT enough to offset the
per-call boundary-copy cost on the common get/set workload (near-parity on Lua,
~3x SLOWER on QuickJS because a byte value must convert to/from an `ArrayBuffer`
every call) and the higher resident memory (~1.64x `_memstore` at 1e6 small
entries). A C store behind the scripting boundary is the wrong shape for a
byte cache whose values live most cheaply inside the runtime.

The recommended follow-up is an in-stdlib O(1) LRU for `_memstore` (an intrusive
linked list replacing the O(n) victim scan), which recovers the ONLY workload
kvmem improved without any boundary or memory penalty. See
[kvmem_negative_result.md § Recommended path forward](kvmem_negative_result.md#recommended-path-forward-a-stdlib-native-o1-lru-for-_memstore).

The original design-only spike follows unchanged, for the record.

---

**Original status (superseded):** design-only. No code until this design is
reviewed and the gates below are accepted. This settles the open questions for a
native C in-process byte cache that would back `hull/kv` (memory) and
`hull/cache` (memory) as a performance drop-in for the pure-Lua/JS `_memstore`,
chosen over CacheLib (see [cachelib_spike.md](cachelib_spike.md)).

## Prime directive: a bit-identical drop-in

The native store MUST be observationally indistinguishable from the current
`stdlib/{lua,js}/hull/kv/_memstore.*` — same values, same miss/nil, same
eviction order, same TTL expiry, same `stats()` counters, same coded errors. The
Lua/JS `_memstore` stays in the tree as the **reference oracle** the conformance
suite diffs against (§11), AND as the always-present fallback when the native
acceleration is not composed (§10). If the native store cannot match the oracle
under a stable clock, it does not ship. Everything below is in service of that
invariant plus a material throughput/latency win — nothing else.

**One deliberate, scoped divergence:** in-process TTL uses a **monotonic** clock
(§6), so under a wall-clock correction the native store is *more* correct than the
current wall-clock `_memstore` (it neither prematurely expires nor resurrects
entries). The two are identical under a stable clock, so the deterministic
conformance vector — short relative TTLs, no clock manipulation — still diffs
clean. This is the only intended behavioral difference, and it is a fix, not a
regression.

Current semantics to preserve (from `_memstore.lua` + `_util.lua`, verified):
entry = {value, exp(ms|nil), bytes, lru-order}; `ENTRY_OVERHEAD = 48` charged per
entry; `bytes = keylen + vallen + 48`; caps `max_bytes` / `max_items`; `evict`
flag; lazy TTL on access; wall-clock `expiry_ms = time.now_ms() + floor(ttl*1000)`;
`MAX_KEY = 1024`; coded errors `invalid_argument` / `capacity_exceeded` /
`unsupported` / `conflict`; `stats = {hits, misses, evictions, expirations, items,
bytes}`; `clear()` wipes data but NOT stats; overwrite resets TTL and bumps MRU;
`incr` preserves TTL and stores the value as a decimal ASCII string.

---

## 1. One core, two policies

**Decision: one store core, one `evict` policy bit** — exactly as the Lua backend
does today. Non-evicting (KV) and evicting (cache) differ only at the capacity
boundary: KV raises `capacity_exceeded` when a cap would be exceeded; cache
LRU-evicts to fit and raises only when a single value exceeds the whole budget.

Refinement over the Lua core: **the intrusive LRU list is maintained only when
`evict = true`.** For a non-evicting KV, LRU order is never read (nothing evicts
by it) and is not observable (`scan` order is unspecified; `stats` has no LRU
field), so a KV `get` skips the O(1) splice entirely — a real read-path win with
zero semantic change. Evicting caches maintain the list.

## 2. Data structures

Three cooperating structures, sized for O(1) ops and stable entries under rehash:

1. **Entry slab (stable).** A grow-only pool of fixed-size `Entry` records
   `{ uint64 hash; uint8 *key; uint32 klen; uint8 *val; uint32 vlen; int64 exp_ms;
   uint32 lru_prev, lru_next; }` addressed by a **stable 32-bit slab index**. Key
   and value bytes are a single owned allocation per entry (`key|val` contiguous),
   freed on drop. A freelist threads dead slots via `lru_next`. Entries never move
   once allocated, so LRU links and borrowed `get` pointers stay valid across a
   table rehash.
2. **Open-addressing index table (buckets hold stable entry indices).** Each
   bucket holds a **stable 32-bit slab index** (+ the cached hash for cheap
   probing), never the entry itself. Probing is **Robin Hood linear probing with
   backward-shift deletion** — no tombstones (backward-shift closes the deleted
   slot's successors' displacement), bounded probe variance, cache-friendly. Grow
   ×2 + full rehash at load factor > 0.85 or if a probe exceeds a hard cap.

   **Borrow-invariance invariant (load-bearing):** rehashing, Robin Hood shifting,
   and backward-shift deletion move **only bucket slots (32-bit indices) in the
   index table**. They NEVER move entry storage: an `Entry` record and its
   `key|val` allocation keep their addresses for the entry's whole lifetime (until
   its own drop). Therefore an intrusive-LRU link (a slab index) and a `get`
   borrow (`ptr,len` into an entry's `val`) remain valid across any number of
   rehashes/shifts. The only thing that invalidates a borrow is a mutating op that
   drops *that* entry (its own `del`/overwrite-shrink/evict/`clear`) — the same
   contract as §3. No shrink on delete in v1 (`clear()` resets to the initial small
   table; optional low-load shrink is a documented follow-up).
3. **Intrusive doubly-linked LRU** over slab indices (head = MRU, tail = LRU).
   `get`/`put`/`incr` splice to head (O(1)); eviction unlinks the tail (O(1)).
   This is the decisive win: the Lua backend picks the victim with an **O(n)**
   min-sequence scan on every eviction; this is **O(1)**.

### 2a. Collision resistance for attacker-controlled binary keys (mandatory)

Keys are arbitrary attacker-supplied bytes, so an unseeded hash is a
hash-flooding DoS: craft colliding keys → O(n) probe chains → O(n²) fill. Every
other Hull surface that parses untrusted bytes is bounds-checked; this one needs
**seed secrecy**.

**Decision: SipHash-1-3, keyed with a per-store 128-bit random seed** drawn at
store creation. SipHash is the standard flooding defense (Rust `HashMap`, Python
dict, Perl). Per-store seeds (not one process seed) also defeat cross-store
collision crafting. The seed is never exposed to script and is scrubbed on store
destroy. Robin Hood caps worst-case probe length even under seed-unknown
collisions; an adversarial test (§11) asserts probe length and per-op time stay
bounded under crafted-collision input. SipHash-1-3 (not 2-4) is the
throughput/security balance modern hash tables use; ~1 cycle/byte, still
flooding-resistant, ~120 lines of C, no dependency.

**Seed source — factor out the existing mbedTLS-independent OS-entropy from
`cap/crypto.c`, do NOT pull `hull/crypto`.** The seed must not drag the TLS/crypto
stack (mbedTLS) into an optional cache feature (§10). Hull already has exactly the
right primitive: `src/hull/cap/crypto.c` contains a self-contained OS-entropy path
— `arc4random_buf` (macOS/BSD) → `getrandom(2)` (Linux) → `/dev/urandom` fallback
(and `getentropy` / `BCryptGenRandom` on cosmo/Windows per `cap/crypto.h`) — which
is **mbedTLS-independent at the call site** (raw OS syscalls, no DRBG). The design
is to **factor that ~30-line core into a standalone leaf helper**
(`hl_os_random_bytes(buf, n)` in a small util TU) that both `cap/crypto.c` and the
native store call, so kvmem links the OS-entropy leaf **without** linking the
crypto/mbedTLS TU. Its (already-written) footprint counts toward the size gate
(§Gates). 16 random bytes per store at creation is the entire entropy demand;
there is no per-op RNG.

## 3. Ownership, copy, and borrow (Lua + QuickJS)

**The store owns copies.** `set(k,v)` copies key and value bytes into a store-
owned allocation (runtime strings can be GC'd; the store must not alias them).

**`get` returns a BORROW.** It yields `(ptr, len)` into the store's own value
allocation, valid **only until the next mutating op on that store** (a later
`set`/`del`/`incr`/`clear`/evict may free it). The binding **copies immediately**
into runtime-owned memory before returning — `lua_pushlstring` (copies) on the
Lua side, `JS_NewArrayBufferCopy`→byte-string on the JS side — exactly the
borrow-copy guard already shipped for the Valkey backend. `scan` delivers each
key as a borrow valid only for the callback; the binding copies inside it. This
unifies the ownership contract across all KV backends (memory, valkey) on one
rule.

Keys/values cross the C boundary as `(ptr,len)`: Lua byte strings and JS byte
strings map identically, so the store is runtime-agnostic and cross-runtime
byte-identical (a prerequisite the conformance suite already enforces for
memory vs sqlite).

## 4. HlAllocator — real deallocation, NOT sealed arenas

**Decision: `HlAllocator` with true per-object free (`hl_alloc_malloc/realloc/
free`). Sealed arenas and `sh_arena` are explicitly wrong here.**

- `hl_seal_arena` is for boot-built **immutable** policy (RW→RO). A cache is the
  opposite: mutable, churning (`set`/`del`/evict/`clear`) for the process life.
  Sealing it would fault on the first write.
- `sh_arena` is a bump allocator with **no per-object free** (only whole-arena
  reset). A cache frees individual entries on every evict/delete/overwrite; a
  bump arena would leak until reset. Wrong tool.

The store therefore holds an `HlAllocator *` and frees every allocation on drop /
overwrite-shrink / `clear` / destroy. Hull's allocator frees by `(alloc, ptr,
size)`; the store already tracks `klen`/`vlen`/table sizes for byte accounting,
so every free size is known. The index table and slab grow via
`hl_alloc_realloc` (old→new size passed). No arena; no leak; ASan-clean churn is
a gate (§11). NULL `alloc` falls back to the process default, per Hull convention.

## 5. Overflow-safe byte accounting + failure behavior

- **Types:** `items`, `bytes`, `max_items`, `max_bytes` are `uint64`/`size_t`.
- **Overflow:** compute `add = klen + vlen + ENTRY_OVERHEAD` with explicit
  `SIZE_MAX` guards (Hull's `> SIZE_MAX/2` convention); if `add` or `bytes + add`
  would overflow, treat as `capacity_exceeded` (fail closed) rather than wrap.
- **Capacity enforcement (KV, `evict=false`):** if a cap would be exceeded, raise
  `capacity_exceeded` — byte-identical to the Lua `"store is full (eviction
  disabled)"`.
- **Eviction (cache, `evict=true`):** evict LRU until it fits; if a single value
  still exceeds the whole `max_bytes` budget, raise `capacity_exceeded`
  (`"value larger than the cache byte budget"`).
- **Allocation failure is DISTINCT from a policy limit.** A `max_bytes`/`max_items`
  breach is `capacity_exceeded` (an app-visible policy outcome). An underlying
  allocator failure (real OOM) is a **resource** failure and gets its own code:
  **`resource_exhausted`** (or the project's existing OOM code if one is later
  standardized). Conflating them would hide "the machine is out of memory" behind
  "your cache is full," which an app handles very differently. `resource_exhausted`
  is a new coded error on the native path; the Lua/JS `_memstore` cannot
  deterministically produce it (a Lua OOM is the 64 MB allocator error), so the
  conformance oracle exercises it via an **injecting `HlAllocator`** (fail the Nth
  allocation) rather than by cross-backend diff.
- **All-or-nothing:** if key, value, or a table-grow allocation fails mid-`set`,
  roll back any partial state (free the half-inserted entry, restore counters) and
  raise `resource_exhausted`. The store is never left partially mutated.

## 6. TTL clock, lazy expiry, LRU order, overwrite, stats parity

- **Clock source: MONOTONIC (elapsed-duration semantics), reusing the existing
  `cap/time.c` primitive.** Hull already exposes a monotonic millisecond clock in
  `src/hull/cap/time.c` (`clock_gettime(CLOCK_MONOTONIC)`, the same source Keel and
  `async/poll.c` use) — proven portable across all four platforms including cosmo.
  In-process memory TTL reuses it: `exp = monotonic_now_ms() + floor(ttl*1000)`. A
  cache entry means "valid for the next N seconds of *elapsed* time"; a wall-clock
  source would let an NTP correction or a manual clock set **prematurely expire**
  live entries or **resurrect** ones that should be gone. Monotonic time is immune
  to both. (No new time helper is needed; the native store calls the existing
  monotonic accessor.)

  This is a **deliberate, documented semantic split** across backends, not a
  parity break: **memory TTL = elapsed-duration** (monotonic, no persistence),
  **SQL/Valkey TTL = wall-clock persistence** (an absolute `expires_at` that must
  survive process restarts and be shared across processes — which *requires* wall
  clock). The prime-directive caveat above already scopes this: memory native and
  the wall-clock Lua `_memstore` differ only under a clock correction, which the
  deterministic conformance vector (short relative TTLs, no clock manipulation)
  does not exercise; under a clock jump the native store is strictly more correct.
  kv_cache.md will state the two TTL semantics explicitly (the Lua `_memstore`'s
  wall-clock TTL is a latent bug the native store fixes and could later backport).
- **Lazy expiry:** every access (`get`/`has`/`incr`/`cas`/`scan`) checks `exp_ms`;
  an expired entry is dropped, counted as an `expiration`, and treated as a miss.
  `cleanup()` sweeps all expired eagerly and returns the count. No background
  timer (matches the Lua backend; hygiene stays caller-driven).
- **LRU order:** `get`/`put`/`incr` move the entry to MRU; eviction takes the
  tail. Overwrite bumps to MRU **before** capacity enforcement so a `set` on an
  existing key never evicts that key (matches the Lua "bump first" logic).
- **Overwrite:** updates value in place, accounts only the byte delta,
  **resets** the TTL to `ttl`/`default_ttl` (a `set` is a fresh value). `incr`
  **preserves** the existing entry's TTL and only changes the value. Both match
  the Lua backend precisely.
- **`incr` value model:** value bytes are parsed as a decimal integer, `+= by`,
  stored back as a decimal ASCII string, using `int64` with the SAME defined
  overflow behavior as Lua 5.4 integer arithmetic (wraparound), so a counter
  reads back identically across backends/runtimes. A non-integer stored value is
  `invalid_argument` (parity with `_util.to_int`).
- **Statistics parity:** `stats()` returns exactly `{hits, misses, evictions,
  expirations, items, bytes}` with the increment points matched 1:1 (hit on live
  `get`; miss on absent OR expired `get`; eviction per LRU drop; expiration per
  lazy/`cleanup` drop). `clear()` wipes entries + `items`/`bytes` but leaves the
  counters (matches Lua). The conformance suite diffs the full `stats()` vector.
- **Stats are PER-STORE, not per-handle.** Counters live on the shared store
  (§7), so two handles opened on the same namespace **aggregate** into one set of
  `hits`/`misses`/`evictions`/`expirations`, and `items`/`bytes` reflect the shared
  contents — `handle.stats()` reports the store, not that handle's own calls. This
  matches the Lua `_memstore` (the `st` table lives on the shared `Store`). It is
  the correct model: the counters describe a keyspace, and same-namespace handles
  are windows onto one keyspace, not independent stores. (A per-handle view would
  require handle-local counters and is explicitly NOT in scope; if ever wanted it
  would be an additive `handle.local_stats()`.)

## 7. Namespace lifecycle — fix the process-lifetime retention

**Current defect (documented in kv_cache.md §"Namespaces are process-lifetime"):**
the Lua `_memstore` keys one store per namespace in a module-level `REGISTRY`
that is **never pruned**, so a namespace derived from unbounded input leaks a
store for the process lifetime.

**Decision: a refcounted native registry reclaimed by explicit handle closure,
with the runtime finalizer as a safety net.** The registry maps `namespace ->
{store, refcount}`. `open` increments (creating the store on first open); the
primary release is an **explicit `handle:close()` / `handle.close()`** that
decrements; the runtime **GC finalizer** (`__gc` in Lua, the class finalizer in
QuickJS) is the **safety net** that decrements for a handle the app dropped
without closing. Double-close and finalize-after-close are no-ops (the handle
nulls its store pointer on first release; every subsequent method fails closed) —
the same idempotent live-handle guard the Valkey `db.open`-style handles use.
Because GC is non-deterministic, apps that churn namespaces should close
explicitly; the finalizer only bounds the worst case.

At refcount 0 the store is destroyed: all entries + `key|val` allocations + the
index table + the SipHash seed (scrubbed) are freed and the registry entry
removed. While any handle is live, same-namespace opens **share** the one store
(the intended in-process sharing; `caps.shared` stays `false` = no cross-process
sharing).

**Reopen after the last handle closes:** the namespace is gone. A subsequent
`open` of that name creates a **fresh, empty** store with the **newly-specified
policy** (this also fixes the current "policy is frozen for the process lifetime"
wart — policy is only frozen while a store is live/shared, not forever). An
in-process memory namespace is therefore **not durable across a full close**,
which is correct for a cache: contents live exactly as long as someone holds the
keyspace. Borrowed `get` values survive a concurrent close because the binding
already copied them out.

This is a genuine improvement the Lua backend cannot easily make (Lua has no
deterministic finalization ordering for shared upvalue state); it is a reason to
prefer the native store. The Lua/JS `_memstore` registry can optionally adopt the
same refcount later, but that is out of scope for this spike.

## 8. Threading — event-loop affinity, no locks, no cross-thread handles

**Decision: single-thread (event-loop) affinity, no internal synchronization,
handles MUST NOT cross worker threads.** The store is an in-process map; sharing
it with the worker pool would require locking (killing the single-thread
throughput that is the whole point) and, worse, a worker's "same namespace" would
be a *different* process-local store — the exact `:memory:` async footgun. So:

- No `async` surface for the memory cache (there is nothing meaningful to
  offload; the ops are nanoseconds). An app needing a worker-visible cache uses a
  SQLite file or `--with=valkey`, documented like the `db.async` + `:memory:`
  caveat.
- Debug builds assert owning-thread access via `HL_THREAD_AFFINITY_CHECKS`
  (`thread_affinity.c`) to catch a handle smuggled across threads.
- No locks → no lock-ordering, no contention; maximal single-thread throughput.

## 9. Capability + coded-error parity

- **caps (exact):** `{ ttl=true, atomic_increment=true, compare_exchange=true,
  scan=true, persistent=false, shared=false, eviction=(evict), transactions=false
  }` — byte-identical to the Lua memory backend. Memory has every op cap, so
  `unsupported` never fires from it (it stays reachable for other backends).
- **Coded errors:** the C ops return an error enum the binding maps to the stdlib
  coded-error shape with the SAME `.code` strings the Lua backend uses:
  `invalid_argument` (key > `MAX_KEY`, non-integer `incr`, bad count args) and
  `capacity_exceeded` (policy limit: full + no-evict, or value > budget). `cas`
  returns a bool (no throw); a `get` miss returns nil/null. These are diffed
  identical across native and reference by the conformance suite.
- **One additive code:** `resource_exhausted` for a true allocator/OOM failure
  (§5), distinct from the policy `capacity_exceeded`. It is native-path-only (the
  Lua backend cannot deterministically emit it) and is covered by the
  injecting-allocator unit test, not the cross-backend diff. This is the only
  coded-error the native store adds; it is additive (no existing code changes
  meaning), and it will be registered in kv_cache.md's coded-error list.
- **Stats are per-store (shared by namespace), not per-handle** — see §6.

## 10. Composition strategy — explicit `--with=cache-native`, NOT base-resident, NOT inferred

**Decision: the native store is an OPTIONAL feature composed only by an explicit
`hull build --with=cache-native`, and the pure-Lua/JS `_memstore` is the
mandatory always-present fallback.** Hull's tailored-binary rule — a produced app
links only what it uses — outweighs any plumbing convenience: a CLI tool or a
stateless service that never opens a memory cache must not carry the
hashmap/SipHash/OS-entropy code, so this does **not** become a base cap module.

**Selection is EXPLICIT, not inferred — following the Valkey / network-backend
precedent.** It is tempting to auto-infer composition (as `needs_image` /
`needs_wasm` do), but that machinery keys off a declared **module capability**
(`HL_MOD_CAP_IMAGE` / `HL_MOD_CAP_WASM`, set when the module is declared), and
**kvmem has no such build-time signal**: `backend="memory"` is a **runtime
argument** to `.open{}`, and `hull/cache` / `hull/kv` are declared even by apps
whose only backend is sqlite or valkey. Inferring from "declares `hull/cache`"
would over-compose (a SQL-only cache app would needlessly pull the native store).
This is exactly the situation of the DB/KV network backends (a `postgres://` /
`redis://` DSN is invisible at build time), which is why **valkey / postgres /
mysql are selected explicitly with `--with=`, never auto-inferred**. kvmem takes
the same path: explicit `--with=cache-native`. (A `--flavor` may still *validate*
against it, as flavors do for other features.)

- **Default (no `--with=cache-native`):** `backend="memory"` uses the stdlib
  `_memstore`, exactly as today — the mandatory fallback. The memory backend
  works with **zero** native support; the base binary is byte-for-byte unchanged.
- **Accelerated (`--with=cache-native`):** `kv.lua` / `cache.lua`'s memory path
  routes to the native store via a thin binding (`hull.kv._native_mem`) instead
  of `_memstore`. The wrapper probes availability once (an optional `require` that
  returns nil when the cap is absent — the soft-absent pattern of an optional `?`
  module) and picks the backend; observable behavior is identical either way
  (the point of §11). The seam is a weak/soft-absent binding, never a hard
  dependency, so an app built without the feature still resolves `backend="memory"`
  through stdlib.

**One core accelerates BOTH memory KV and memory cache.** Because `hull/kv`
(memory, non-evicting) and `hull/cache` (memory, evicting) are the SAME store core
differing only by the `evict` bit (§1), composing `--with=cache-native` once
speeds up **both** memory paths — `kv.open{backend="memory"}` and
`cache.open{backend="memory"}` alike. There is no separate KV-vs-cache feature;
the single native core backs both, so the size cost is paid once and the
acceleration is shared.

**Footprint gate (must measure before merge):** the composed delta = `size` of
(app built `--with=cache-native`) − (same app without) on arm64 Darwin.
**Gate: ≤ ~20 KB** for (kvmem.o + the OS-entropy helper + siphash + the two
runtime bindings). If it exceeds the gate, the composition still keeps it off
every other binary, but the number is reported and reviewed. See also the
resident-memory gate below.

## 11. Test, fuzz, and benchmark plan

- **Conformance oracle:** extend `tests/e2e_kv.sh` Part A so the deterministic
  vector runs against the native memory store AND the Lua/JS `_memstore`, and
  asserts `native == reference` (the harness already diffs memory vs sqlite and
  Lua vs JS). Covers get/set/del/has/incr/cas/scan/clear/cleanup, TTL expiry,
  eviction order under `max_items`/`max_bytes`, overwrite TTL reset, `incr` TTL
  preserve, the full `stats()` vector, and coded-error `.code` parity. This is the
  semantic gate.
- **Unit tests (`test_kvmem.c`, utest):** open-addressing invariants (Robin Hood
  displacement, backward-shift delete leaves no hole), rehash preserves entries +
  LRU links + borrowed pointers, byte accounting exact after churn, refcounted
  namespace reclaim (store freed at refcount 0), overflow guards, all-or-nothing
  on simulated alloc failure (an injecting `HlAllocator`).
- **Adversarial hash test:** insert N keys crafted to collide under an unseeded
  reference hash; assert seeded SipHash keeps max probe length and per-op time
  bounded (no O(n) blowup). Include a second seed to show the crafted set does
  not transfer.
- **Fuzz (`fuzz_kvmem.c`, libFuzzer):** random op sequences (set/get/del/incr/
  cas/scan/clear with random binary keys/values, caps, TTLs) checked against a
  simple model (a shadow map) for behavioral equivalence + invariant assertions
  (no leak, no UAF, byte accounting == recomputed, LRU integrity). 60s in CI like
  the other fuzzers.
- **Sanitizers:** the whole suite under ASan+UBSan (mutable free path is exactly
  UAF/double-free/leak territory) and MSan (uninit). Non-negotiable.
- **Benchmarks — measured SEPARATELY per runtime (Lua and QuickJS), never
  blended.** The workload matrix, each vs the corresponding `_memstore`:
  1. **peak lookup** (hot `get` on a warm set) — the easy win, reported but not
     sufficient on its own;
  2. **mixed get/set** (e.g. 80/20 and 50/50, the realistic cache shape);
  3. **TTL churn** (sets with short TTLs + reads that trigger lazy expiry);
  4. **eviction-heavy** (sustained over-cap sets forcing continuous LRU eviction —
     where the O(1) tail-unlink vs the Lua O(n) min-scan dominates).
  Run at 1e3 / 1e5 / 1e6 keys, small + large values. The performance gate is
  judged on the mixed / TTL-churn / eviction-heavy numbers **for each runtime
  independently**, not on peak lookup alone.

## Ship gates (all must pass, or it does not ship)

1. **Semantic:** conformance oracle shows ZERO observable difference vs the
   Lua/JS `_memstore` under a stable clock (values, nil/miss, eviction order, TTL,
   full `stats()`, error `.code`s), with the single scoped, documented
   monotonic-TTL divergence (§6) that only manifests under a clock correction.
   Non-negotiable — the point is a drop-in, not a new backend.
2. **Performance (per runtime, real workloads):** materially faster than the
   `_memstore` **measured separately for Lua and for QuickJS** — target **≥ 2×**
   on the **mixed get/set, TTL-churn, and eviction-heavy** workloads (§11), not on
   peak lookup alone — plus the qualitative eviction win **O(n) → O(1)**. If it is
   not materially faster on both runtimes across those workloads, it does not ship.
3. **Binary footprint:** composed-size delta **≤ ~20 KB** on arm64 Darwin
   (kvmem + OS-entropy helper + siphash + both bindings), measured (§10).
4. **Resident memory:** for a fixed workload (say 1e6 small entries), measured
   peak **RSS ≤ the Lua/JS `_memstore` for the same workload**, and the store's
   accounted `bytes` must stay within a small factor (**≤ 1.5×**) of the actual
   heap it holds — i.e. byte accounting is honest and the native store is no
   hungrier than the interpreter tables it replaces. A faster cache that costs
   materially more RAM does not ship.
5. **Composability:** pure portable C, no new deps (no crypto/mbedTLS pull — §2a),
   no new authority; the memory backend retains the stdlib `_memstore` fallback
   with the feature absent (§10); builds on all four platforms including cosmo;
   ASan/MSan/fuzz clean.

If any gate fails, keep the shipped Lua/JS `_memstore` and do not merge the native
store. The native store is an optimization; it earns its place only by being
faster **and** identical **and** small **and** memory-lean **and** composable.

## Ratified decisions (locked; this doc is the design of record)

- [x] **Robin Hood** open addressing, **buckets hold stable entry indices**,
      **backward-shift deletion** (no tombstones); rehash/shift move only bucket
      slots, never entry storage or borrows (§2).
- [x] **SipHash-1-3, per-store seed from the smallest OS-entropy primitive**
      (`getentropy`/`getrandom`/`/dev/urandom`) — **no `hull/crypto`/mbedTLS
      pull**; add `hl_os_random_bytes` if none exists, counted in the size gate
      (§2a).
- [x] **Borrow-on-`get` + binding-copy** contract, unified with Valkey (§3).
- [x] **`HlAllocator` real per-object free** — explicitly NOT sealed arenas / not
      `sh_arena` (§4).
- [x] **`resource_exhausted`** for allocator/OOM failure, distinct from the policy
      **`capacity_exceeded`** (§5, §9).
- [x] **Monotonic** in-process TTL (elapsed-duration); SQL/Valkey stay wall-clock
      persistence — a documented split, not a parity break (§6).
- [x] **Per-store** stats (shared-namespace handles aggregate), not per-handle
      (§6, §9).
- [x] **Refcounted namespace registry**, reclaimed by **explicit close + GC
      finalizer safety net**; reopen-after-last-close = fresh empty store with new
      policy (§7).
- [x] **Event-loop-only**, no locks, no async surface, no cross-thread handles
      (§8).
- [x] **Explicit `--with=cache-native` composition** (Valkey/network-backend
      precedent — selection is a runtime backend string, not a build-time module
      cap, so NOT auto-inferred and NOT base-resident); one native core
      accelerates both memory KV and memory cache; stdlib `_memstore` is the
      mandatory always-present fallback when the feature is absent (§10).

## Gates as MEASURED (concluded — see kvmem_negative_result.md)

Measured on arm64 Darwin, Hull v0.13.0, `HL_ENABLE_CACHE_NATIVE=1`:

- [x] **Semantic — PASS:** conformance oracle diff = 0, Lua == JS byte-identical.
- [ ] **Performance — FAIL:** mixed Lua 1.11× / JS 0.29×; ttl Lua 1.10× / JS
      0.36×; eviction Lua 93× / JS 33×. Below the ≥ 2× bar on mixed and
      TTL-churn for both runtimes (native is SLOWER than `_memstore` on JS
      get/set). Only eviction-heavy wins.
- [x] **Binary footprint — PASS:** single-runtime composed delta ~15-17 KB.
- [ ] **Resident memory — FAIL:** native 156 MB vs `_memstore` ~95 MB (1.64×) at
      1e6 small entries; accounted `bytes` ~0.5× of real heap.
- [~] **Composability — not decisive:** `_memstore` fallback verified,
      ASan/UBSan + fuzz clean; cosmo not exercised (moot given the two failures).

Two gates failed structurally, so the native store did **not** ship. The
`_memstore` remains the only memory backend. This section is retained as the
measured record; the ship/no-ship rule ("if any gate fails, keep `_memstore` and
do not merge") was applied as written.

Related: [kv_cache.md](kv_cache.md) (semantic layer + the retention defect + the
two TTL semantics), [cachelib_spike.md](cachelib_spike.md) (why not CacheLib),
[features_and_flavors.md](features_and_flavors.md) (optional-composition
taxonomy).
