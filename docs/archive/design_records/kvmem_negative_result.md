# `cap/kvmem.c` native cache store: negative result (concluded, not shipped)

**Status:** CONCLUDED. The native in-process C cache store designed in
[kvmem_design.md](kvmem_design.md) was implemented on a throwaway branch,
measured against its five ratified ship gates, and **did not ship**. Two gates
failed decisively and structurally. This document records the methodology, the
exact numbers, the root causes, and the decision, so the experiment is not
re-run without new evidence.

The pure-Lua/JS `_memstore` remains the sole memory backend for `hull/kv` and
`hull/cache`. No implementation code, feature registration, runtime bindings, or
`--with=cache-native` packaging was merged.

## What was built (and where it lives)

A complete Phase 1-3a implementation exists only on the local, unpushed branch
`feat/kvmem-native` (preserved for reference, not proposed for merge):

- `cap/kvmem.c`: Robin Hood open addressing over a stable chunked entry slab,
  backward-shift deletion, O(1) intrusive LRU, SipHash-1-3 keyed per store from
  OS entropy, monotonic TTL, `HlAllocator` real free, overflow-safe uint64 byte
  accounting. 15 unit tests + an adversarial constant-hash suite + a libFuzzer
  differential harness, all ASan/UBSan clean; scan-build and cppcheck clean.
- The `os_random` entropy leaf (factored out of `cap/crypto.c`, mbedTLS-free)
  and `utils/siphash.c` (validated against published SipHash-2-4 vectors via a
  parameterized `sip_cd`).
- Lua + JS runtime bindings and a weak/strong composable-feature registration
  seam (`cap/kvmem_feature.c`), mirroring the TUI feature seam.
- A conformance oracle proving semantic equivalence with `_memstore` (Gate 1).
- The gate-measurement harness (`tests/fixtures/kvmem_bench/`).

The implementation is sound. It is the *architecture* (a C store behind the
scripting boundary for a byte cache) that the gates rejected.

## Environment

- Hardware: Apple Silicon (arm64), macOS (Darwin 25.3.0).
- Base: Hull v0.13.0 (`main` at fd400af3).
- Build: `make HL_ENABLE_CACHE_NATIVE=1` (monolithic: both the native store and
  the pure `_memstore`, and both runtimes, in one binary so A/B runs share a
  process and a warm allocator).
- Instruction limit: the default 100M. The direct `hull app.lua` run path pins
  it (neither `--max-instructions` nor `HULL_MAX_INSTRUCTIONS` is honored on that
  path), so the benchmark reports THROUGHPUT (ops/ms) with each store sized under
  its own budget and one workload per process, rather than equal-op wall time.
  `_memstore`'s O(n) eviction would otherwise exhaust a shared budget before
  native (C, O(1)) finished warming.

## Methodology

Three ratified workloads (kvmem_design.md §11), each store measured separately
per runtime, keys pre-built into arrays so the timed loop is allocation-free and
measures the store rather than string construction:

- **mixed** get/set, 80/20, working set 20k keys, small values.
- **ttl-churn**: half the ops are `put(k, v, ttl)`, half `get`.
- **eviction-heavy**: a store capped at 500 items, sustained distinct-key inserts
  so the LRU victim path runs on nearly every insert (`_memstore` does an O(cap)
  min-scan; native does an O(1) tail unlink).

Resident memory (Gate 4): one store populated with 1e6 small entries
(`key<i>` -> `value<i>`), peak RSS via `/usr/bin/time -l`, one store per process.

Harness: `tests/fixtures/kvmem_bench/{app.lua,app.js,mem.lua}` on the
`feat/kvmem-native` branch. Reproduce with a `HL_ENABLE_CACHE_NATIVE=1` build:

```
hull tests/fixtures/kvmem_bench/app.lua --no-sandbox -- mixed   # | ttl | evict
hull tests/fixtures/kvmem_bench/app.js  --no-sandbox -- mixed   # | ttl | evict
/usr/bin/time -l hull tests/fixtures/kvmem_bench/mem.lua --no-sandbox -- ref    1000000
/usr/bin/time -l hull tests/fixtures/kvmem_bench/mem.lua --no-sandbox -- native 1000000
```

## Gate results

| Gate | Target | Result | Verdict |
|------|--------|--------|---------|
| 1 Semantic | conformance diff = 0, Lua == JS | native == `_memstore` byte-identical across the full op vector + eviction order + `stats()` (`3 1 0 0 7 365` both runtimes); cross-runtime serialized vector identical | PASS |
| 2 Performance | >= 2x on mixed AND ttl AND evict, per runtime | see table below | **FAIL** |
| 3 Footprint | <= ~20 KB composed delta | single-runtime object footprint ~15-17 KB (see below) | PASS (borderline) |
| 4 Resident memory | peak RSS <= `_memstore` (1e6 small entries) | native 156 MB vs `_memstore` ~95 MB = **1.64x** | **FAIL** |
| 5 Composability | portable C, no new deps, `_memstore` fallback, 4 platforms, sanitizers/fuzz | fallback verified (default base: 0 kvmem/siphash symbols, both runtimes fall back); ASan/UBSan + fuzz clean; cosmo build not exercised | not decisive |

### Gate 2 (performance), speedup = native throughput / `_memstore` throughput

| Workload | Lua | JS |
|----------|-----|-----|
| mixed get/set | 1.11x | 0.29x (native SLOWER) |
| ttl-churn | 1.10x | 0.36x (native SLOWER) |
| eviction-heavy | 93.5x | 32.6x |

Native is materially faster ONLY on eviction-heavy workloads. On the common
cache shape (get/set-heavy) it is near-parity on Lua and roughly 3x SLOWER on
QuickJS.

### Gate 3 (footprint), object TEXT+DATA, arm64 Darwin

| Object | TEXT+DATA |
|--------|-----------|
| `cap_kvmem.o` (core) | 9012 B |
| `siphash.o` | 528 B |
| `lua_rt_mod_kvmem.o` (Lua bridge) | 5548 B |
| `js_mod_kvmem.o` (JS bridge) | 7742 B |

A real `--with=cache-native` app links the core + siphash + ONE runtime bridge:
~15.1 KB (Lua) / ~17.3 KB (JS), under the ~20 KB gate. (`os_random` is
base-resident, shared with crypto, so it adds no delta.) The monolithic
flag-on-vs-base linked-binary delta is ~35 KB because it counts BOTH bridges plus
link padding; that is not the composed figure. Gate 3 passes, but it is moot
given Gates 2 and 4.

## Root causes (why these are structural, not tuning)

**Gate 2 (the scripting boundary is wrong for a byte cache).** The `hull/kv` and
`hull/cache` value type is a byte string. `_memstore` stores the runtime's own
string object and returns it with ZERO copy: a get is a single interpreter hash
lookup. The native store must cross the C boundary on every call and pay an
O(len) cost `_memstore` never pays:

- Lua: `lua_get_buffer` to read the key, `lua_pushlstring` to COPY the value out
  of the store into a fresh Lua string on every get. Net ~1.1x (the C boundary
  cancels the algorithmic parity).
- QuickJS: the binding deals in `ArrayBuffer`, so the adapter converts every key
  and value string <-> `ArrayBuffer` per call (`toBuf` allocates a `Uint8Array`
  and byte-copies; `fromBuf` rebuilds a byte string). This conversion is forced
  by QuickJS binary safety (`JS_NewStringLen` UTF-8-mangles arbitrary bytes, so a
  byte string cannot round-trip through it). Net ~0.3x: three times SLOWER than a
  plain QuickJS `Map`.

For get/set-heavy traffic the interpreter primitive is already near-optimal, and
no amount of tuning removes a per-call boundary copy that the in-runtime store
does not have. Native's only decisive win, eviction, comes precisely from the one
place `_memstore` is algorithmically weak (an O(n) min-scan for the LRU victim),
and that weakness lives ENTIRELY inside the interpreter, where it can be fixed
without any C.

**Gate 4 (the flat store is not leaner here).** For 1e6 tiny entries the native
store's per-entry key and value allocations (macOS `malloc` carries ~16-32 B of
overhead each) plus Robin Hood power-of-2 table over-allocation add up to MORE
resident memory than 1e6 compact, interned interpreter table entries. The
accounted byte figure (64.6 MB) is roughly half the store's real heap, i.e. the
accounting under-reports the true footprint. A flat C store beating a tuned
interpreter allocator on many tiny objects would require a memory-layout redesign
(inline-small-value, SoA) with uncertain payoff.

## Decision

**Do not ship the native store. Do not renegotiate the gates.** Semantic
correctness (Gate 1) and an O(1) eviction win (part of Gate 2) were not enough to
offset the boundary-copy cost on the common get/set workload (Gate 2) and the
higher resident memory (Gate 4). The five gates existed precisely to reject an
"optimization" that is not faster on the realistic shape and not memory-lean; they
did their job.

Per kvmem_design.md's own rule: "If any gate fails, keep the shipped Lua/JS
`_memstore` and do not merge the native store."

## Recommended path forward: a stdlib-native O(1) LRU for `_memstore`

The single workload where kvmem won (30-90x) is exactly where `_memstore` is
weak: its eviction victim search is an O(n) scan over every entry. That is a
property of the current pure-Lua/JS implementation, NOT of the language boundary.
Fixing it inside the stdlib keeps values in the runtime (no boundary copy, no
`ArrayBuffer` conversion, no extra RSS) while removing the one real deficiency.

Proposed (separate effort, own design + PR):

- Add an intrusive doubly-linked LRU list threaded through the existing entry
  records in `stdlib/{lua,js}/hull/kv/_memstore.{lua,js}`. Each entry gains
  `prev`/`next` links (Lua table fields / JS object fields); the store keeps
  `head`/`tail` references.
- `get`/`put` on an existing key unlink-and-move-to-head in O(1). `make_room`
  evicts `tail` in O(1) instead of scanning for the minimum `seq`. This deletes
  the `seq` counter and the O(n) min-scan entirely.
- Byte and item accounting, TTL semantics, and the conformance behavior stay
  exactly as they are (the current `_memstore` is the semantic oracle native was
  diffed against, so parity is already characterized).
- Measure against the SAME eviction-heavy workload used here. Target: turn the
  O(n) min-scan into O(1) and recover most of the 30-90x on eviction WITHOUT
  regressing mixed/ttl (which stay in-runtime and already win) and WITHOUT any
  RSS increase.

This is the correct place to spend the effort: it addresses the only workload
kvmem improved, for a fraction of the complexity, with none of the boundary or
memory penalties. Do NOT pursue the QuickJS string fast-path or a C memory-layout
redesign for kvmem unless a stdlib O(1) LRU is first shown to be unable to meet
the eviction requirement.

## Related

- [kvmem_design.md](kvmem_design.md): the original design spike + the five gates
  (now annotated CONCLUDED).
- [kv_cache.md](kv_cache.md): the semantic layer (`hull/kv`, `hull/cache`).
- [cachelib_spike.md](cachelib_spike.md): why not CacheLib (the prior decision
  that led here).
