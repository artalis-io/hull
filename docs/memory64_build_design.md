# `hull build` for Memory64 compute plugins — decision record (#336)

**Status:** DESIGN, pre-implementation. Split from #334. Owns the production
`hull build` / `stdlib/cli/lua/hull/build.lua` AOT path for Memory64 compute
plugins. #318 shipped the runtime dispatch; #334 shipped runtime mapped spans;
this issue makes `hull build` of a Memory64 *plugin* actually work.

## 0. The investigation changed the premise — read this first

#336 was framed around a supposed problem: `build.lua` compiles every compute AOT
with `--enable-shared-heap`, and a `--enable-shared-heap` mem64 AOT "segfaults when
no heap is attached" (attributed to #318's `echo64`). **That premise does not hold
for Hull's shipping targets**, and the shared-heap-policy question it implied
largely dissolves. Two findings:

### F1 — the crash is a 32-bit-TARGET-only WAMR bug; Hull AOT-targets only 64-bit

Root-caused in WAMR (`vendor/wamr`, `WAMR-2.4.1`). When a `--enable-shared-heap`
AOT runs with **no heap attached**, every linear-memory access still runs the
shared-heap range check `addr >= start_off && addr <= end_off - n + 1`. WAMR makes
that range **empty** so the check is always false and linear memory is used:

- `core/iwasm/aot/aot_runtime.c:2134-2138` (instantiation) and the
  `wasm_runtime_detach_shared_heap_internal` paths set the sentinel:
  ```c
  #if UINTPTR_MAX == UINT64_MAX          /* 64-bit target */
      extra->shared_heap_start_off.u64 = UINT64_MAX;
  #else                                   /* 32-bit target */
      extra->shared_heap_start_off.u32[0] = UINT32_MAX;   /* u32[1] left unset */
  #endif
  ```
  with an explicit comment: *"when shared heap is disabled, we set the start off to
  UINT64_MAX in 64-bit target … so … the formula will be false, we don't need to
  check whether the shared heap is enabled."* This is **deliberate, correct**
  heap-less handling.
- The AOT codegen (`core/iwasm/compilation/aot_emit_memory.c:398`,
  `aot_llvm.c:1579`) loads that field with `offset_type = is_target_64bit ? I64 : I32`.

The bug the agent found is real **only on a 32-bit TARGET running a Memory64
module**: there the sentinel writes just `u32[0]=UINT32_MAX` (leaving `u32[1]`
unset) and the codegen loads an I32, so a ≥4 GiB 64-bit `start_offset` compares
`>=` a zero-extended `0xFFFFFFFF` → true → the shared-heap branch → `NULL + offset`
→ segfault. **On a 64-bit target the field is a full `UINT64_MAX` and the codegen
loads I64 → the range is empty → safe.**

**Hull AOT-targets ONLY `x86_64` and `aarch64`** — `build.lua` hard-rejects
anything else (`unsupported --target arch '…' (expected x86_64 or aarch64)`,
build.lua ~L362), the runtime `wasm_arch_suffix()` returns only `x86_64`/`aarch64`
(else NULL), and cosmo is a fat `x86_64` + `aarch64` APE. So the 32-bit-target bug
is **out of scope for every artifact Hull can produce**.

### F2 — #318's "shared-heap crash" was never isolated (two variables changed)

#318 observed `exit 139` on the FULL `test_wasm` suite with an `echo64.aot` built
`--enable-shared-heap`, then dropped `--enable-shared-heap` **and** switched to
`--filter` isolation in the same step; the isolated no-shared-heap run passed. The
crash was therefore never isolated to "shared-heap + no heap" on a 64-bit target,
and its attribution to that combination **contradicts WAMR's deliberate 64-bit
design (F1)**. It was most likely a full-suite artifact or mis-attribution.

**Net:** the transparent policy Hull already wants — compile every plugin with
shared-heap support, attach a real heap only when a call requests spans/segments —
is **safe on Hull's 64-bit targets with NO WAMR patch, NO empty-heap runtime shim,
and NO manifest opt-in**.

### F3 — CONFIRMED by a controlled single-variable experiment

Before locking, an isolated repro varied **only** `--enable-shared-heap` on the
heap-less path (same `echo64.wasm`, same `memory64_aot_dispatch` test, same
leading-only-wildcard filter — no fixture/filter/suite change), on both 64-bit AOT
targets (temporary CI job `mem64-sharedheap-repro`, reverted after):

| arch    | wamrc flags                                   | heap-less mem64 dispatch | exit |
|---------|-----------------------------------------------|--------------------------|------|
| x86_64  | `--opt-level=3 --bounds-checks=1`             | `[ OK ]` (600 µs)        | 0    |
| x86_64  | `--opt-level=3 --bounds-checks=1 --enable-shared-heap` | `[ OK ]` (650 µs) | 0 |
| aarch64 | `--opt-level=3 --bounds-checks=1`             | `[ OK ]` (446 µs)        | 0    |
| aarch64 | `--opt-level=3 --bounds-checks=1 --enable-shared-heap` | `[ OK ]` (398 µs) | 0 |

`--enable-shared-heap` on a heap-less Memory64 AOT **passes on both x86_64 and
aarch64** — no segfault. This confirms F1/F2: the #318 exit-139 was NOT caused by
`--enable-shared-heap` on a 64-bit target (it was an unisolated full-suite
artifact). The transparent policy is **locked with no WAMR patch**.

## 1. Options, in the order requested — and the outcome

1. **(d) Fix WAMR's Memory64 heap-less path.** The fix (make the sentinel + codegen
   always 64-bit under Memory64) is real but only matters on a **32-bit target**,
   which Hull never AOT-targets. **Not required for Hull.** (If Hull ever adds a
   32-bit AOT target, revisit as a `patches/wamr/0006-…`; noted, not planned.)
2. **(a) Empty-heap runtime attach.** Unnecessary — 64-bit init already yields the
   empty (safe) range.
3. **(c) Manifest opt-in.** Unnecessary — no per-plugin divergence needed.
4. **(b) Reject Memory64 spans/segments.** No — #334 proved they work with a heap
   attached; rejecting would regress a shipped capability.

**DECISION: retain the transparent policy unchanged.** #336 reduces to a
build-tool bug-fix plus an E2E, gated by an empirical confirmation that the
heap-less 64-bit path is in fact safe (§3, the E2E's plain-plugin case).

## D1 (LOCKED) — remove the bogus `--enable-memory64` argument + stale comment

- `stdlib/cli/lua/hull/build.lua` (~L1885): delete the
  `if mem64 then table.insert(wamrc_args, 3, "--enable-memory64")`. wamrc
  auto-detects Memory64 from the module's `(memory i64)` type; the flag prints
  usage and fails the compile (confirmed in #318/#334). Keep the `mem64` detection
  only for the human-readable `(memory64)` build log line.
- `stdlib/cli/lua/hull/aot_cache.lua` (~L108): fix the stale
  `mem64_flag — 1 if --enable-memory64 will be passed` comment. The AOT cache key
  still includes the mem64 bit (a mem64 module and a wasm32 module of the same
  bytes would AOT differently), but the comment must not reference the dead flag.

## D2 (LOCKED) — shared-heap policy: TRANSPARENT, one path for all call kinds

`build.lua` keeps `--enable-shared-heap` on **every** compute AOT (mem64 and
wasm32 alike). Runtime behavior is unchanged and identical across call kinds:

- **Plain mem64 call** (no spans, no segments): no heap attached; the AOT's
  shared-heap range is the empty 64-bit sentinel (F1) → linear memory → safe.
- **Segment-backed mem64 call** (`compute.segment`): the module's segment chain is
  attached (the existing path) → real shared-heap range → reads resolve.
- **Span-backed mem64 call** (`spans={…}`): the per-invocation span chain is
  attached and torn down per call (#309/#313 lifecycle, #315 teardown) → real
  range → reads resolve. Runtime-proven by #334's `memory64_span_readback`.

No per-kind flag divergence, no dummy allocation.

## D3 (LOCKED) — the E2E is also the empirical confirmation of F1/F2

A `hull build` E2E (a new `tests/e2e_compute_memory64.sh`, or a leg of the compute
E2E) that builds a REAL app whose `compute/` holds a committed `(memory i64)`
plugin `.wasm` (authored like `spanread64.wat` via `wat2wasm --enable-memory64`,
SHA-pinned; NO clang-wasm64 dependency), runs `hull build` (which AOT-compiles it
through the fixed `build.lua`), and executes the produced binary:

- **Case A — plain Memory64 plugin, heap-less call.** The decisive confirmation of
  F1/F2: a mem64 plugin doing ordinary linear-memory work, called with no spans and
  no segments, MUST run correctly (not segfault) on **both x86_64 and aarch64** (the
  existing E2E arch coverage). If this crashes, F1/F2 are wrong for Hull's targets →
  STOP, reopen option (d), root-cause with a backtrace (the WAMR map above is the
  starting point), and do NOT ship the transparent policy.
- **Case B — Memory64 span/segment consumer.** A mem64 plugin that reads a
  `compute.segment` (and/or a mapped span) end-to-end through the built binary,
  confirming the attached-heap path works through the real pipeline (the E2E
  analogue of #334's unit gate).

Both cases assert `hull build` SUCCEEDS (D1: no bogus flag) and the run produces
correct output. A must-NOT-skip CI leg gates them (they need wamrc for AOT; mirror
the #318/#334 must-not-skip pattern — fail if the mem64 case takes a skip path).

## D4 (LOCKED) — interpreter vs AOT, and instance lifecycle (no change needed)

- **Interpreter:** unchanged. `memory64_requires_aot` (cap/wasm.c) rejects a mem64
  module on the interpreter call path before any shared-heap logic; `hull build`
  always AOT-compiles `compute/*.wasm`, and a dev-mode interp call of a mem64
  plugin already fails closed with that error. No new rejection code.
- **Pooled + persistent instances:** unchanged. `shared_heap_start_off` is
  per-instance and initialised to the safe sentinel at instantiation (F1); a
  pooled/persistent instance that never attaches a heap keeps that sentinel; the
  span teardown (#315 detach-before-destroy on drain, and per-call span detach)
  already restores the chain-free state. The one-chain-per-instance rule is
  untouched (this issue attaches no new chains). Case A of the E2E exercises the
  pooled path (a plain `compute.call`), Case B the segment/span-attached path.

## D5 — docs on landing

Flip the `hull build` caveat: once the E2E is green, `CLAUDE.md` /
`docs/wamr_architecture.md` / `docs/roadmap.md` say `hull build` of a Memory64
compute plugin is **supported** (transparent shared-heap policy, CI-gated), and
this record's status → IMPLEMENTED. If Case A crashes, record the F1/F2 refutation
and pursue option (d) instead.

## Non-goals

- A WAMR `0006` patch (the 32-bit-target fix) — out of scope; Hull AOT-targets only
  64-bit. Revisit only if a 32-bit AOT target is added.
- A dummy/empty shared-heap runtime attach, or a manifest opt-in — unnecessary
  (F1).
- clang-`wasm64` plugin authoring in the E2E — the committed `.wat`→`.wasm` fixture
  path (#334) is the reliable authoring route.
- Changing the wasm32 compute path in any way.
