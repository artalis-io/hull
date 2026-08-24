# `--with=cachelib` design spike (decision doc, no implementation)

**Status:** spike / recommendation. **Verdict: do NOT vendor or compose Meta's
CacheLib as a Hull feature.** Build the already-documented native C cache store
(`cap/kvmem.c`) instead. This document is the design-first evaluation requested
before any implementation is authorized; it does not add code.

CacheLib (github.com/facebook/CacheLib) is Meta's pluggable in-process C++
caching engine (DRAM + optional NVM/SSD hybrid tiering, LRU/LFU/2Q eviction,
slab allocation). It is genuinely excellent at what it does. It is also, on every
axis that matters to Hull's distribution model, a poor fit. The six dimensions
below are each disqualifying on their own; together they are conclusive.

## TL;DR

| Dimension | Finding | Fit |
|---|---|---|
| Dependency footprint | folly + fbthrift + fizz + wangle + mvfst + ~15 transitive C++ libs | ✗ |
| Static-link viability | folly static-init/singletons; getdeps builds shared; tens of MB | ✗ |
| Platform matrix | Linux only (no macOS, no cosmo; aarch64 untested) | ✗ |
| Licensing / SBOM | Apache-2.0 (legally OK) but ~doubles the SBOM with libs Hull avoids | ⚠ |
| Allocator model | self-owned slab manager + background threads + direct SSD I/O | ✗ |
| Narrow C ABI | none — template-heavy C++ API with folly types | ✗ (shim needed) |

**Recommendation:** ship `cap/kvmem.c` (a small, dependency-free C hashmap +
intrusive LRU + byte accounting) as the fast local-cache backend for
`hull/cache`. It covers the realistic Hull use case (bounded, evicting,
in-process byte cache) on all four platforms including cosmo, honoring
`HlAllocator` + sealed arenas, with **zero** new SBOM surface. Keep the
just-shipped `--with=valkey` backend as the distributed-cache answer. Revisit a
hybrid DRAM+SSD tier only if a concrete workload demands it, and then via a
purpose-built minimal C module or a lightweight C library — never folly.

## Context: what Hull requires of a feature

The composable-feature bar (docs/features_and_flavors.md) and Hull's invariants:

- **All dependencies vendored, no external deps** (CLAUDE.md "Dependencies"). The
  distribution is one self-contained binary; the SBOM is ~15 tight vendored libs.
- **Four platforms**: `linux-x86_64`, `linux-aarch64`, `darwin-arm64`, and the
  cosmo APE. `darwin-arm64` is a first-class target (and the maintainer's dev
  machine); cosmo must build from a fat APE.
- **Reproducible builds, W^X, no runtime codegen, sealed-arena policy,
  `HlAllocator` discipline, secure-zero of key material, controlled threading
  under a kernel sandbox.**
- Features are **static archives** composed at `hull build --with=`; a C++
  feature (like DuckDB) additionally needs a system compiler + a C++ runtime.

CacheLib is architected against almost all of these.

## 1. Dependency footprint

CacheLib's own `getdeps.py` pulls the Meta C++ platform stack:

- **Primary:** folly, fbthrift, fizz, wangle, mvfst (Thrift RPC, TLS 1.3, QUIC).
- **Secondary (a dozen+):** boost, libevent, lz4, snappy, zlib, OpenSSL,
  libunwind, libsodium, glog, gflags, fmt, sparse-map, xxHash, googletest,
  and typically **jemalloc** for production.

That is ~15-20 third-party libraries for one cache backend, several of which
**duplicate or conflict with Hull's existing vendored stack**:

- **OpenSSL** vs Hull's **mbedTLS** — a second, larger TLS stack in a binary
  that ships mbedTLS-only by design (and that the pure-compute flavor drops
  entirely). Two TLS libraries in one static binary invites symbol collisions
  and doubles the crypto attack surface.
- **libsodium** vs Hull's **TweetNaCl** — a second NaCl implementation.
- **lz4 / snappy / zlib** vs Hull's **miniz** — a second/third compression stack.
- **fmt / glog / gflags / boost** — large C++ runtime machinery Hull otherwise
  has zero of.

Vendoring CacheLib means either bundling all of this (bloat + SBOM + conflicts)
or an unbounded "bring your own system libs" story that breaks Hull's
self-contained, reproducible distribution. Both are unacceptable.

## 2. Static-link viability

- CacheLib's supported build path (`getdeps.py`) produces **shared** libraries;
  a fully-static folly+fbthrift link is **not a documented or supported
  configuration**.
- folly relies on **static-initialization-order-sensitive globals**
  (`folly::Singleton`, thread-local registries, static constructors, runtime CPU
  feature detection). Reliable static linking of folly is a known-hard problem;
  it fights Hull's boot model (explicit, auditable init order before the
  sandbox/seal phases — see the `__attribute__((constructor))` guidance in the
  C-audit skill).
- Expected binary cost: **tens of MB** added, versus Hull's whole ~6.5 MB full
  binary and the ~48 KB pure-C valkey feature archive.
- **cosmo (APE) is impossible**: folly is glibc/Linux-specific and cannot target
  the fat multi-OS APE. So even in the best case this feature could never reach
  one of Hull's four shipped targets.

## 3. Platform matrix

CacheLib's `BUILD.md` lists tested platforms as **Ubuntu 18.04/20.04/22.04,
CentOS 8, Debian 10/11** — Linux only. **No macOS, no Windows**, and aarch64 is
not addressed (unofficial at best). Compiler floor is **C++20**.

Hull publishes features for all three **native** platforms
(`linux-x86_64`, `linux-aarch64`, `darwin-arm64`). CacheLib would, optimistically,
cover only `linux-x86_64` (+ `linux-aarch64` unverified), i.e. a **strictly
narrower matrix than every existing Hull feature**, and would **not build on the
maintainer's own macOS** — a severe DX and CI regression (the feature's e2e could
not run in the macOS CI lane).

## 4. Licensing / SBOM

- **License is fine legally.** CacheLib and folly are **Apache-2.0**, which is
  one-way compatible *into* AGPL-3.0; the transitive deps are permissive
  (BSL-1.0 boost, ISC libsodium, BSD glog/gflags/snappy, MIT fmt, OpenSSL's
  Apache-2.0). No copyleft conflict.
- **SBOM is the problem.** Hull's SBOM (`hull sbom`, the signed release manifest)
  today covers a tight, audited vendored set. CacheLib **roughly doubles** it
  with ~15-20 components — each needing version pinning, provenance, and ongoing
  CVE tracking — and pulls in exactly the large-surface libraries (OpenSSL,
  boost) Hull deliberately excludes. Composed-feature attestation
  (docs/composed_feature_signing.md) would have to sign a vastly larger archive,
  and the supply-chain review burden per release grows accordingly.

## 5. Allocator model

Fundamental impedance mismatch. Hull threads a **pluggable `HlAllocator`** (+
`sh_arena`, + `hl_seal_arena` RW→RO policy sealing, + `secure_zero`) through
every subsystem, runs a **controlled** thread pool, and enforces a **kernel
sandbox** (pledge/unveil/seatbelt).

CacheLib is a **self-contained memory manager**: it owns a large slab-allocated
region (often GBs), runs its **own background threads** (eviction, slab
rebalancing, and — in hybrid mode — NVM flushing), keeps global state in
`folly::Singleton`, and in NVM/hybrid mode performs **direct SSD I/O**
(io_uring / libaio). None of this can be routed through `HlAllocator`, sealed at
boot, or cleanly bounded by the sandbox without bespoke grants. CacheLib does not
*use* an allocator you give it; it *is* the allocator/evictor/IO-manager. That is
the opposite of Hull's model.

## 6. Narrow C ABI

There is **no C ABI**. CacheLib's public interface is template-heavy C++
(`CacheAllocator<CacheTrait>`, `CacheItem`, typed `ItemHandle`s) exposing folly
types. To sit behind Hull's narrow byte-oriented backend vtable (the
`HlKvBackend` / cache-store shape) it needs a **hand-written C++ shim TU** that
instantiates a concrete `CacheAllocator`, translates `get/set/del/cas/incr/scan`
to item handles, and hides every folly/C++ type from the C boundary — the DuckDB
`--with` pattern (`cxx = true`, system compiler, `-lstdc++`/`-lc++`), but larger,
plus folly's load-time static-init/singleton/thread setup running before Hull's
controlled boot + sandbox. Feasible in principle, disproportionate in practice.

## Recommendation & decision gate

**Do not authorize a `--with=cachelib` implementation.** Instead:

1. **Build `cap/kvmem.c`** — the native C cache store already flagged as a
   follow-up in [docs/kv_cache.md](kv_cache.md) (open-addressed hashmap +
   intrusive LRU + byte accounting), presenting the same store interface the
   Lua/JS memory backend already exposes to `hull/kv` + `hull/cache`. It is the
   correct, in-model answer for a fast bounded in-process cache: a **base cap
   module** (small new C, always wanted, no new authority — the taxonomy's
   in-base tier), dependency-free, on all four platforms incl. cosmo, honoring
   `HlAllocator` + sealed arenas, **zero** new SBOM surface. This is a
   performance drop-in, not a correctness prerequisite (the stdlib memory backend
   already byte-accounts, so behavior is already covered/tested).
2. **Keep `--with=valkey`** (shipped) as the distributed / cross-process cache.
3. **Defer hybrid DRAM+SSD tiering** unless a concrete Hull workload demands it.
   If it ever does, spike a **purpose-built minimal C NVM-admission module** or a
   single-purpose C library — never folly/CacheLib — as its own separate
   evaluation.

**What would change this recommendation:** a CacheLib release that (a) ships a
stable **C ABI**, (b) supports a **fully-static** build with **no folly runtime
singletons**, (c) officially supports **macOS + aarch64**, and (d) drops the
OpenSSL/boost/thrift stack for a cache-only core. None of these are on CacheLib's
roadmap; it is designed for Meta's Linux fleet, not a portable self-contained
embedder. Until then, `cap/kvmem.c` is the answer.

## Sources

- [CacheLib README](https://github.com/facebook/CacheLib) — overview, "pluggable
  in-process caching engine", Apache-2.0.
- [CacheLib BUILD.md](https://github.com/facebook/CacheLib/blob/main/BUILD.md) —
  tested platforms (Ubuntu/CentOS/Debian, no macOS/Windows), C++20, secondary
  deps (boost, libevent, lz4, snappy, zlib, ssl, libunwind, libsodium, glog,
  gflags, fizz, wangle, fmt, sparse-map, xxHash).
- [CacheLib Installation](https://cachelib.org/docs/installation/) +
  [getdeps notes](https://github.com/facebook/CacheLib/blob/main/README.md) —
  `getdeps.py`, primary deps folly/fbthrift/wangle/fizz/mvfst, boost.
- [facebook/folly](https://github.com/facebook/folly) — dependency surface +
  static-init characteristics.
- CacheLib allocator headers
  ([CacheAllocatorConfig.h](https://github.com/facebook/CacheLib/blob/main/cachelib/allocator/CacheAllocatorConfig.h),
  [CacheItem.h](https://github.com/facebook/CacheLib/blob/main/cachelib/allocator/CacheItem.h))
  — C++ template API (no C ABI).
- Hull: [docs/kv_cache.md](kv_cache.md) (native C cache store follow-up),
  [docs/features_and_flavors.md](features_and_flavors.md) (feature bar),
  [docs/composed_feature_signing.md](composed_feature_signing.md) (attestation),
  CLAUDE.md (vendored-deps invariant, four-platform matrix, taxonomy).
