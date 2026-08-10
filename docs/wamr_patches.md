# WAMR out-of-tree patches + test-toolchain pins

Hull vendors WAMR as a git submodule (`vendor/wamr`) pinned to a fixed commit. Any
Hull-specific change to WAMR is carried as a **numbered out-of-tree patch** applied
into an isolated staged build tree (`build/wamr-staged/`) - the submodule checkout
is NEVER left dirty and is NEVER silently mutated. A deterministic
verify-base + dry-run step gates the build so a WAMR upgrade that makes a patch
stale fails loudly.

**Pinned WAMR base commit:** `c3a78cd159e59c86ac4543308bd676ff78d30a93`
(WAMR-2.4.1-218-gc3a78cd1). Every patch below is expressed against this base; the
apply step refuses to run if `vendor/wamr` is not at this commit.

## Test-toolchain bootstrap pin - WASI-SDK 25.0

**Trust basis: this is a repository-maintainer trust decision, NOT verification
against an upstream-published checksum.** WASI-SDK's GitHub release publishes no
`SHA256SUMS`/per-asset digest, and the pinned WAMR revision's own
`.github/actions/install-wasi-sdk-wabt` downloads WASI-SDK without verifying a
checksum. WASI-SDK's documented install method is "download the versioned GitHub
release artifact for your platform" (https://github.com/WebAssembly/wasi-sdk).
We therefore bootstrap-pin the artifact: download it ONCE over TLS from the exact
official release URL, record its identifying metadata + our computed SHA-256 here,
and require every later local/CI fetch to match this SHA-256 exactly. The pin is
never silently refreshed if the upstream asset changes - a mismatch is a hard
failure to be investigated by a maintainer.

This toolchain is **test-only**: it builds the `.wasm`/`.aot` fixtures for WAMR's
upstream unit-test suites. It is not part of Hull's shipped build.

| field | value |
|-------|-------|
| release tag | `wasi-sdk-25` |
| asset name | `wasi-sdk-25.0-arm64-macos.tar.gz` |
| platform | arm64 macOS (this host) |
| GitHub asset id | `212790089` |
| size (bytes) | `104133504` |
| asset updated_at | `2024-12-12T02:52:00Z` |
| official URL | `https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-arm64-macos.tar.gz` |
| **SHA-256 (maintainer bootstrap pin)** | `e1e529ea226b1db0b430327809deae9246b580fa3cae32d31c82dfe770233587` |
| bundled clang | `19.1.5-wasi-sdk` (target `wasm32-unknown-wasi`) |
| cache location | `$HOME/.hull/toolcache/wasi-sdk-25.0-arm64-macos` |

Linux/x86_64 CI would pin `wasi-sdk-25.0-x86_64-linux.tar.gz` separately (its own
asset id + SHA-256) the first time it is provisioned, following the same process.

## Test-toolchain bootstrap pin - LLVM 18.1.8 (for wamrc / AOT fixtures)

Same maintainer-trust bootstrap-pin model as WASI-SDK (no upstream digest is
published by the LLVM release; we download once over TLS and pin our computed
SHA-256, never silently refreshed). **Required because** the pinned WAMR revision
compiles its AOT compiler (`wamrc`) against LLVM **18.1.8**
(`build-scripts/build_llvm.py` -> `llvmorg-18.1.8`); the host's default Homebrew
LLVM 22 removed the `PGOOptions` API `aot_llvm_extra.cpp` uses and cannot build
wamrc. wamrc is needed to produce the `.aot` fixtures for the upstream unit-test
baseline AND to validate the read-only patch's AOT enforcement site.

**Strictly a Phase 0 TEST/BUILD tool.** It is isolated under
`$HOME/.hull/toolcache`, is not installed globally, does not touch `/opt` or the
user's default compiler, and is passed to the WAMR/wamrc build ONLY via explicit
`LLVM_DIR` (and `PATH` where needed). It **must not** enter Hull's runtime
dependency graph or shipped SBOM - Hull's runtime links no LLVM; wamrc is an
optional side-loaded AOT tool.

| field | value |
|-------|-------|
| release tag | `llvmorg-18.1.8` |
| asset name | `clang+llvm-18.1.8-arm64-apple-macos11.tar.xz` |
| platform | arm64 macOS (this host) |
| GitHub asset id | `188772339` |
| size (bytes) | `838639376` |
| asset updated_at | `2024-08-28T04:35:04Z` |
| official URL | `https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/clang+llvm-18.1.8-arm64-apple-macos11.tar.xz` |
| **SHA-256 (maintainer bootstrap pin)** | `4573b7f25f46d2a9c8882993f091c52f416c83271db6f5b213c93f0bd0346a10` |
| `clang --version` | `clang version 18.1.8` |
| `llvm-config --version` | `18.1.8` |
| cache location | `$HOME/.hull/toolcache/clang+llvm-18.1.8-arm64-apple-macos11` |
| `LLVM_DIR` (wamrc build) | `<cache>/lib/cmake/llvm` |

Linux/x86_64 CI would pin `clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-*.tar.xz`
separately (own asset id + SHA-256) by the same process.

WABT: only provisioned if the baseline configuration or the selected upstream
tests actually require `wat2wasm` (the shared-heap fixtures are C, so likely not).
If required, `wabt-1.0.37` is pinned by the same documented bootstrap process.

## Test-toolchain pin - WABT 1.0.37 (for the memory.init fixture)

`memory.init` needs a passive data segment, which C-to-wasm (WASI-SDK clang)
cannot express, so the read-only matrix fixture is authored as a checked-in
`.wat` (`tests/unit/shared-heap/wasm-apps/test_readonly.wat`) and assembled by
`wat2wasm`. WABT is provisioned by the same isolated, pinned model, **but unlike
WASI-SDK / LLVM it is verified against an upstream-published checksum**: the
WABT release publishes a per-asset `.sha256`, which we downloaded and matched
exactly (not merely a maintainer bootstrap pin).

| field | value |
|-------|-------|
| release tag | `1.0.37` |
| asset name | `wabt-1.0.37-macos-14.tar.gz` |
| platform | arm64 macOS (this host) |
| GitHub asset id | `234231869` |
| size (bytes) | `3779425` |
| asset updated_at | `2025-03-03T17:32:51Z` |
| official URL | `https://github.com/WebAssembly/wabt/releases/download/1.0.37/wabt-1.0.37-macos-14.tar.gz` |
| **SHA-256** | `a996907cbdf1bdcd56d66d382dd1b031450bb7326bbfd0f43a04486b620bed1c` |
| checksum basis | **matches upstream-published** `wabt-1.0.37-macos-14.tar.gz.sha256` (asset id `234231873`) |
| `wat2wasm --version` | `1.0.36 (git~1.0.37)` |
| cache location | `$HOME/.hull/toolcache/wabt-1.0.37` |

Strictly a **test tool**: it assembles one test fixture and never enters Hull's
runtime graph or SBOM. The fixture is regenerated deterministically by CMake
only when a pinned `-DWAT2WASM_BIN=<path>` is supplied (no `find_program()`
fallback, so the build never depends on an ambient `wat2wasm`); otherwise the
committed `test_readonly.wasm` is used as-is. Regenerate manually with:

```
$HOME/.hull/toolcache/wabt-1.0.37/bin/wat2wasm \
  tests/unit/shared-heap/wasm-apps/test_readonly.wat -o test_readonly.wasm
```

## Interpreter enforcement checkpoint (patch 0002, site 1) - COMPLETE

Read-only store enforcement is wired for every store family via a store-only
`CHECK_SHARED_HEAP_STORE_OVERFLOW` (loads keep the read path untouched):

- scalar i32/i64 + narrow, and every SIMD `v128.store` / `storeN_lane`, route
  through `CHECK_MEMORY_OVERFLOW_STORE` (defined in BOTH the SW and HW
  bound-check macro branches);
- `memory.init` / `memory.copy`(dst) / `memory.fill` route through
  `CHECK_BULK_MEMORY_OVERFLOW_STORE` on the SW branch AND through an inline
  read-only dest trap on the HW branch (the bulk ops don't use the macro there);
  `memory.copy` **source** is left a plain read (read-only source permitted);
- the read-only trap is gated on non-zero length, so a zero-length bulk op
  writes nothing and does not trap on permission alone (WASM bounds semantics),
  while an invalid zero-length address still traps via the normal bounds check.

**Validated in BOTH bound-check configurations, identical matrix:**

| config | build | shared_heap_test | full suite |
|--------|-------|-----------------:|-----------:|
| HW bound check (WAMR default) | `build/wamr-ut/all` | 33 / 0 | 103 / 0 |
| SW bound check (Hull ships `WAMR_DISABLE_HW_BOUND_CHECK`) | `build/wamr-ut-sw` | 33 / 0 | 103 / 0 |

The 11 new `shared_heap_test` cases cover: Detail-2 reject; all 9 scalar store
families trap; all 5 SIMD store/lane families trap; explicit load-only returns
the correct value; `memory.copy` read-only-source-OK vs read-only-dest-trap;
`memory.fill` + `memory.init` dest trap; zero-length no-trap-on-valid +
trap-on-invalid; boundary-straddle + uint32-overflow fail closed with backing
bytes unchanged; a successful invocation after every trap class; and a writable
equivalent for every operation class. SIMD is enabled in the shared-heap test
build (`WAMR_BUILD_SIMD 1`, matching Hull) so the v128 families are reachable.

The baseline suites are unchanged (aot 57, memory64 12, interpreter 1); the
shared-heap suite grew 22 -> 33, so the total is 92 -> 103, still 0 failures.

## AOT increment 1 (site 2 groundwork) -- versioned and lockstep

The AOT read-only plumbing is NOT a behaviorally-inert metadata add: inserting
`shared_heap_read_only` into `AOTModuleInstanceExtra` shifts `common` and every
later field, which is a **versioned ABI break**. It is handled as one:

- **Deterministic field offset.** `read_only` is placed immediately after
  `shared_heap` (offset **40**), BEFORE `common`. `common` carries
  conditionally-compiled fields whose size differs between the wamrc build and
  the runtime build; a position after it made wamrc and the runtime disagree on
  `read_only`'s offset (wamrc read 220, the runtime had it elsewhere) and the
  mismatched entry-load faulted the chain path into a spurious OOB. Offset 40 is
  stable because all preceding fields are unconditional.
- **ABI audit.** The AOT codegen reads AOTModuleInstanceExtra by `offsetof`
  only (no literal offsets), and audited to touch **no field past `read_only`**
  (only `base_addr_adj`/`start_off`/`end_off`/`shared_heap`/`read_only`, all
  <= 40). So the `common` shift changes no generated offset -- it is purely a
  versioned layout change. `aot_runtime.c` (compiled into BOTH wamrc and the
  runtime) now static-asserts `offsetof(read_only)==40` AND
  `offsetof(common)==48`, plus the pre-existing 0/8/16/24/32 asserts.
- **Version bump landed atomically.** `AOT_CURRENT_VERSION` 6 -> 7 (`core/config.h`)
  is part of the SAME change as the layout shift. wamrc stamps version 7; the
  runtime's `aot_compatible_version` accepts only 7. A version-6 artifact (old
  layout) is refused before any code runs, and a version-7 artifact is refused
  by an unpatched (version-6) runtime. Proven by
  `test_aot_version7_enforced_version6_rejected` (loads v7, patches the version
  field to 6 in-buffer, asserts the load is rejected with a version error).
- **Matched-heap permission.** The chain helper
  `wasm_runtime_check_and_update_last_used_shared_heap` gained a `read_only`
  out-param and writes the MATCHED heap's `read_only` into the func-context cache
  together with `base_addr_adj`/`start_off`/`end_off`, so the permission always
  belongs to the heap that formed the address (never a stale value from another
  attached span). The enforcement (site-2) reads this cache in the
  `app_addr_in_cache_shared_heap` convergence block, reached by both the direct
  cache-hit and the chain-hit, before native-address formation.

**Deterministic fixture generation.** Ad-hoc looped `wamrc` invocations silently
produced stale/mismatched `.aot` (a wamrc drifted from the runtime with nothing
rejecting it). Replaced by `tests/unit/shared-heap/gen_readonly_fixtures.sh`:
explicit input/output paths, checked exit status per call, rejects missing /
zero-length / wrong-magic / wrong-version / duplicate-output artifacts, and
records sha256 + AOT version per file into `fixtures.manifest`. Wired into CMake
(`-DWAMRC_BIN=<path>` -> `gen_ro_aot_fixtures` target); identical locally and in
CI. Full version-7 baseline (regenerated through the script): shared_heap 34,
aot 57, memory64 12, interpreter 1 = **104 / 0**.

## AOT increment 2 (site 2 enforcement) -- COMPLETE, both bound-check configs

The read-only store trap is emitted in the AOT codegen:

- **`is_store` threaded** through `aot_check_memory_overflow` ->
  `aot_check_shared_heap_memory_overflow` and the bulk equivalent
  (`check_bulk_memory_overflow` -> `aot_check_bulk_memory_shared_heap_memory_overflow`).
  Loads pass `false` (byte-identical to the pre-enforcement baseline); the store
  opcodes pass `true`: scalar `i32/i64.store`+narrow, `f32/f64.store`, SIMD
  `v128.store`/`store_lane` (simd_load_store.c), bulk `memory.copy`(dest only,
  source stays a load) / `memory.fill`, and the stringref encodes. Atomics
  (SHARED_MEMORY, compiled out in Hull; a read-only heap is never an atomic
  target) are threaded for signature-correctness only.
- **Permission branch dominates address formation.** `build_shared_heap_store_ro_trap`
  is emitted inside `app_addr_in_cache_shared_heap` -- the convergence block both
  the direct cache-hit and the chain-hit reach -- BEFORE
  `build_get_maddr_in_cache_shared_heap`. IR (verified by `opt -passes=verify`):
  `shared_heap_ro = load i8` -> `is_readonly = icmp ne` ->
  `br %is_readonly, got_exception, shared_heap_store_ok`, and
  `maddr_cache_shared_heap` forms only inside `shared_heap_store_ok`. The maddr
  phi incoming is the current block (the store-permitted block), not the
  cache block. Ordinary linear-memory stores are untouched (the trap is only in
  the shared-heap branch).
- **Matched-heap permission.** Because the chain helper wrote the matched heap's
  `read_only` into the cache together with `base_addr_adj`, and the trap reads
  that cache in the same block that forms the address from `base_addr_adj`, the
  permission always belongs to the heap that forms the address. Proven for BOTH
  cache directions by the alternating-heaps test (writable-cached -> read-only
  store traps; read-only-cached -> writable store succeeds), AOT and interp.
- **Bulk zero-length** is gated: `build_shared_heap_store_ro_trap` AND-s the
  read-only condition with `len_nonzero` for bulk ops, so a zero-length op does
  not trap on permission alone.
- **`memory.init`** is a runtime call (`aot_memory_init`), not codegen, so its
  write-path trap lives in `aot_runtime.c`: after `wasm_runtime_validate_app_addr`
  (which serves reads), a new chain-aware `wasm_runtime_shared_heap_is_readonly`
  (zero-length-gated) traps a read-only destination.

**Full Phase 0 evidence, both bound-check configurations** (regenerated v7
fixtures via `gen_readonly_fixtures.sh`):

| suite | HW bound check | SW bound check (Hull ships) |
|---|---:|---:|
| shared_heap_test | 40 / 0 | 40 / 0 |
| aot_test | 57 / 0 | 57 / 0 |
| memory64_test | 12 / 0 | 12 / 0 |
| interpreter_test | 1 / 0 | 1 / 0 |
| **total** | **110 / 0** | **110 / 0** |

The AOT matrix (`test_readonly.aot`, version 7) covers all store families trap,
loads return correct values, `memory.copy` read-only-source-OK / dest-trap,
`memory.fill`/`memory.init` dest-trap, zero-length no-trap, boundary/overflow
fail-closed with backing unchanged, writable-heap all-classes, and
recover-after-trap -- identical to the interpreter matrix, plus the
alternating-heaps stale-cache test in both directions. LLVM verification passes
on representative scalar/SIMD/copy/fill modules; `memory.init` is exercised by
the runtime test.

## Unpatched upstream baseline (Phase 0, before any permission change)

Established on the staged tree carrying ONLY the test-harness path-portability
change (patch 0001), read-only permission patch (0002) ABSENT. Confirmed the
staged tree differs from pinned WAMR (`c3a78cd1`) by **only** the 3 portability
CMakeLists (`tests/unit/{custom-section,running-modes,shared-heap}/CMakeLists.txt`).

- **Host/tools:** arm64 macOS (Darwin 25.3.0), cmake 3.29.0, gtest via
  FetchContent, WASI-SDK 25.0 (clang 19.1.5), LLVM 18.1.8 -> wamrc 2.4.3.
- **Config:** top-level `tests/unit` driver, `-DWASI_SDK_DIR=<cache>`,
  `-DLLVM_DIR=<llvm-18.1.8>/lib/cmake/llvm`, `-DCMAKE_BUILD_TYPE=Release`,
  platform darwin, `WAMR_BUILD_TARGET=AARCH64`. Fixtures: `.wasm` via WASI-SDK
  clang, `.aot` via wamrc `--enable-shared-heap|--enable-shared-chain`.

| suite (unit target) | required-set role | tests | failures | machine output |
|---|---|------:|---------:|---|
| `shared_heap_test` | shared heap (interp + AOT); includes bulk-memory-into-heap cases | 22 | 0 | `wamr-baseline-c3a78cd1/shared_heap.json` |
| `memory64_test` | Memory64 (interp + AOT) | 12 | 0 | `…/memory64_test.json` |
| `interpreter_test` | interpreter | 1 | 0 | `…/interpreter_test.json` |
| `aot_test` | AOT (embeds the LLVM-18.1.8 AOT compiler) | 57 | 0 | `…/aot_test.json` |
| **total** | | **92** | **0** | (durable: `$HOME/.hull/toolcache/wamr-baseline-c3a78cd1/`) |

- **bulk memory:** no dedicated upstream unit dir; covered by the shared-heap
  suite's `*_chain_rmw_bulk_memory*` cases (green above).
- **SIMD:** no dedicated upstream unit suite exists. The read-only patch (0002)
  touches the `v128.store*` write path, so patch 0002 MUST add its own SIMD
  store-into-RO-heap trap coverage (Hull-tree C-API test + a WAMR-unit `TEST_F`),
  since there is no upstream SIMD suite to regress against.

Both gates to proceed to patch 0002 are met: every required baseline suite is
GREEN, and the staged-tree diff is portability-only.

## Carriage + apply/verify tooling

`scripts/wamr_apply_patches.sh` applies 0001+0002 onto a clean checkout of the
pinned base into `build/wamr-patched` (vendor/wamr is never mutated) and is the
deterministic gate: **verify-base** (submodule at the pinned commit, not dirty),
**stale-patch** (each patch's sha-256 matches the recorded pin), **offset**
(`git apply --check` exact-context), **tamper** (`--reverse --check`), and
**unexpected-source** (the changed/new file SET must equal exactly what the
patches declare -- else fail). `make wamr-patch` produces the tree; `make
wamr-patch-check` is the `--dry-run` CI gate (wired into `.github/workflows/ci.yml`,
fails on stale/offset/unexpected-source).

**A stock `make` builds against the patched tree automatically.** `WAMR_DIR`
defaults to `build/wamr-patched` (an `?=` default), and the apply is a stamped,
locked prerequisite of `$(BUILDDIR)` (see `mk/vendor/wamr.mk`), so the first
compile stages + patches the tree before any Hull source that includes the
patched WAMR headers is built. Nothing is dormant behind an override; a clean
`make` and every CI build compile the enforced runtime. The apply is skipped for
read-only goals (`make help`, `make clean`, the registry checks) because none of
them pull `$(BUILDDIR)`, and concurrent/parallel applies serialise on an atomic
`mkdir` lock.

### `WAMR_DIR=vendor/wamr` is a development escape hatch only

Overriding `WAMR_DIR=vendor/wamr` compiles the pristine, UNPATCHED submodule.
This is supported ONLY for development (e.g. bisecting an upstream regression
against the pristine tree). It is safe in Phase 0 because a shared heap with no
read-only span is memory-safe on the unpatched runtime -- the patches add an
*enforcement* path that nothing yet depends on for safety.

**A future mapped-spans layer MUST refuse to build against an unpatched
runtime.** Once read-only zero-copy spans exist, the write-trap enforcement is a
safety invariant, not an optimisation: on `WAMR_DIR=vendor/wamr` a read-only
span would be silently *writable*, a security regression. When that layer lands
it must hard-error out of the build when a spans-enabled target is requested
against `vendor/wamr` (gate it in the `else` branch of the `WAMR_DIR` ifeq in
`mk/vendor/wamr.mk`).

## Hull validation against the staged patched tree (Phase 0 gate)

| check | result |
|---|---|
| clean `make` (arm64 macOS) against `build/wamr-patched` | PASS (exit 0) |
| compute unit suites (`test_wasm`, `test_wasm_buffer`) | 58/58, 12/12 |
| compute e2e (`e2e-compute`: interp + AOT + shared segments + `app.main`) | 20/20 |
| ASan/UBSan compute suites | 58/58, 12/12, no ASan/UBSan report |
| version-7 AOT end to end through Hull's wamrc -> runtime (AOT cache regen) | PASS (20/20 incl. AOT) |
| pure-compute + DB-off flavor (`HL_ENABLE_HTTP=0 HL_ENABLE_DB=0`) | PASS (exit 0) |
| `vendor/wamr` pristine at pinned base after all work | 0 dirty files |
| final staged audit (`build/wamr-patched` == pinned + 0001 + 0002) | PASS (reverse-check + 22-file set) |

Host survival after a trap is proven at the WAMR layer (the recover-after-trap /
alternating-heaps unit cases, 110/110 both bound-check configs) and at the Hull
layer (the gas-exhaustion + compute suites run to completion). Static analysis
of the vendored WAMR change is by clean `-Wall` compilation of the AOT compiler +
LLVM `opt -passes=verify`; Hull's own scan-build/cppcheck targets `src/` (not
vendored code) and is unaffected. MSan is Linux/clang-only (not runnable on this
macOS host); Cosmopolitan and the Linux ASan/MSan legs run in CI. Feature
composition (`--with=`) does not touch the compute/WAMR path.

## Patch 0002 compatibility hardening (three ABI/lockstep details)

Before wiring the enforcement sites, three compatibility hazards a new
permission field introduces were closed:

**1. Every `SharedHeapInitArgs` construction site zero-initializes.** A struct
field appended to a public init-args type is garbage at any caller that
partially initializes a stack instance. Audited every construction site in the
staged WAMR tree AND Hull's source:

| site | init form | `read_only` value |
|---|---|---|
| `tests/unit/shared-heap/shared_heap_test.cc` (21 sites) | `SharedHeapInitArgs args = {};` | 0 (writable) |
| `samples/shared-heap/src/{main,shared_heap_chain}.c` | `memset(&heap_init_args, 0, sizeof(...))` | 0 (writable) |
| Hull `src/hull/cap/wasm_data.c:415` | `memset(&heap_args, 0, sizeof(heap_args))` | 0 (writable) |

All sites zero/`{}`-initialize, so `read_only` defaults **false** (writable)
everywhere -- no existing caller changes behavior. Coverage: the existing
`shared_heap_test` rmw cases create writable heaps via `= {}` and store into
them; they would fail if zero-init yielded `read_only=true`, so they are the
regression guard for the default.

**2. `read_only=true` on a non-preallocated (runtime-owned) heap is rejected,
not silently ignored.** `read_only` is only coherent for a caller-owned
`pre_allocated_addr` buffer (a file-backed PROT_READ mapping); the
runtime-managed malloc heap must stay writable. `wasm_runtime_create_shared_heap`
now fails closed (`LOG_WARNING` + `NULL`) when `read_only && !pre_allocated_addr`,
so an incoherent request is a hard configuration error rather than a permission
that is dropped on the floor.

**3. Patched wamrc and runtime stay lockstep; incompatible AOT artifacts are
rejected.** Two independent mechanisms:

- *Hull build identity (cache invalidation).* Hull's compute-AOT cache key
  already folds in `wamrc_version = "wamrc-sha256=" .. sha256(wamrc contents)`
  (`stdlib/cli/lua/hull/aot_cache.lua`). The patched wamrc is a different binary
  (different bytes) than an unpatched one, so its sha256 differs, so the cache
  key differs: the cache can never serve an artifact built by one across a build
  using the other. This is content-hash-of-the-compiler, the strongest possible
  identity -- no change needed, documented here as the invalidation guarantee.
- *WAMR loader version (hard rejection).* The AOT enforcement codegen (the
  aot_emit_memory.c site) emits a load of `shared_heap_read_only` at its struct
  offset; an `.aot` carrying that codegen must NOT load against an unpatched
  runtime that lacks the field. `AOT_CURRENT_VERSION` (`core/config.h`, currently
  `6`, checked by `aot_compatible_version` in `aot_loader.c`) is bumped to `7`
  **in the same hunk that adds the codegen**, so a patched-wamrc `.aot` (version
  7) is rejected with a version mismatch by any unpatched runtime (which expects
  6), and vice versa. Safe for Hull because Hull always builds wamrc + runtime
  from the same staged tree, and regenerates `.aot` through the wamrc-sha-keyed
  cache. The version bump is coupled to the AOT-visible codegen and to the
  layout change together (see below), so the two never diverge.

**Accurate ABI statement.** `AOTModuleInstanceExtra.read_only` is NOT merely
appended. It is INSERTED at **deterministic offset 40**, immediately after
`shared_heap` (offset 32) and **before `common`** -- so it SHIFTS `common` and
every field after it. This placement is required for correctness: a struct-END
placement lands *after* `common`, whose size differs between the wamrc build and
the runtime build (conditionally-compiled members), which made wamrc and the
runtime disagree on the field's offset (wamrc read 220, the runtime had it
elsewhere) and faulted the AOT chain path into a spurious trap. Offset 40 is
stable because every preceding field is unconditional. The shift is a genuine
ABI break, made safe by the atomic `AOT_CURRENT_VERSION` 6->7 bump (version-7
lockstep, proven v6 rejection) and by static assertions compiled into BOTH the
wamrc and runtime configs: the preserved fixed-prefix asserts
(`stack_sizes==0`, `shared_heap_base_addr_adj==8`, `shared_heap_start_off==16`,
`shared_heap_end_off==24`, `shared_heap==32`) plus
`offsetof(shared_heap_read_only)==40` and `offsetof(common)==48` (the shifted
position). The audited AOT codegen reads no field past `read_only`, so the shift
changes no generated offset -- it is a pure versioned layout change.

## Patch series (against the pinned base)

Extracted into `patches/wamr/`. Base commit
`c3a78cd159e59c86ac4543308bd676ff78d30a93`. Applying 0001 then 0002 to a fresh
checkout of the base yields a tree byte-identical to the validated staged source
(proven by `scripts/wamr_apply_patches.sh` reverse-check + changed-file-set
audit). Deterministic apply/verify + CI dry-run live in that script.

| # | file | bytes | purpose | patch sha-256 |
|---|------|------:|---------|---------------|
| 0001 | `0001-tests-unit-wasi-sdk-dir-overridable.patch` | 1528 | TEST-HARNESS ONLY: wrap the hard-coded `set(WASI_SDK_DIR "/opt/wasi-sdk")` in `tests/unit/{custom-section,running-modes,shared-heap}/CMakeLists.txt` with `if(NOT WASI_SDK_DIR)` so a cached/env `WASI_SDK_DIR` overrides it. No test source/fixture/flag/assertion change. | `423beeae0e94454381ce0d805e9985c5cd94e14e511981c629af186676411698` |
| 0002 | `0002-shared-heap-readonly-permission.patch` | 81832 | Read-only shared-heap permission: metadata (`SharedHeapInitArgs.read_only`, `WASMSharedHeap.read_only`, interp/AOT extra caches), AOT ABI versioning (`AOT_CURRENT_VERSION` 6->7 + deterministic offset 40 + asserts), interpreter enforcement (`wasm_interp_fast.c`), AOT enforcement (`aot_emit_memory.{c,h}`, `aot_llvm.{c,h}`, `simd/simd_load_store.c`, `aot_emit_stringref.c`, `aot_runtime.c` memory.init), the deterministic fixture generator, the `test_readonly.wat` fixture source, and the full trap/matrix tests. | `310706eb6a36ae33756c85997b6599b4855d279e350ba7dae7c5b0353a6c8177` |

The two patches are kept SEPARATE so the test-harness portability change (0001)
and the security change (0002) are independently reviewable. Generated `.wasm`/
`.aot` fixtures are NOT in the patches (they are produced deterministically at
build time from `test_readonly.wat` by pinned WABT + wamrc via the CMake wiring /
`gen_readonly_fixtures.sh`).

**Files touched by 0002 (16 WAMR source files + 2 test files + 2 new fixtures):**
`core/config.h`; `core/iwasm/include/wasm_export.h`;
`core/iwasm/interpreter/{wasm_runtime.h, wasm_interp_fast.c}`;
`core/iwasm/common/{wasm_memory.c, wasm_memory.h, wasm_runtime_common.c, wasm_runtime_common.h}`;
`core/iwasm/aot/{aot_runtime.c, aot_runtime.h}`;
`core/iwasm/compilation/{aot_emit_memory.c, aot_emit_memory.h, aot_llvm.c, aot_llvm.h, aot_emit_stringref.c, simd/simd_load_store.c}`;
`tests/unit/shared-heap/{CMakeLists.txt, shared_heap_test.cc}`; new
`tests/unit/shared-heap/gen_readonly_fixtures.sh` and
`tests/unit/shared-heap/wasm-apps/test_readonly.wat`.
