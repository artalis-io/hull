# Memory64 cap-layer dispatch — decision record (#318)

**Status:** IMPLEMENTED (#318, PR #335). D4.1–D4.3 landed; D4.4 + the `hull build`
mem64 path deferred to #334. Scope: activate Hull's already-written
Memory64 dispatch by making detection work, via a public WAMR accessor rather than
by exposing WAMR internals to Hull TUs. One standalone PR on #318.

## Problem (recap)

Everything downstream of detection is already plain C keyed on `mod->is_memory64`
(`cap/wasm.c`: 8-cell marshalling ~L1078, `memory64_requires_aot` ~L922, io_max
widening ~L1378). The **only** dark path is detection: the `#if WASM_ENABLE_MEMORY64`
block (~L697) reads `mem->is_memory64` off the internal `WASMMemoryInstance` via
`../interpreter/wasm_runtime.h`, and `WASM_ENABLE_MEMORY64` is defined only for the
vendored WAMR objects, never for Hull TUs. So `mod->is_memory64` is always 0.

## D1 (LOCKED) — public accessor, not internal header

Rejected: adding `-DWASM_ENABLE_MEMORY64=1` + WAMR internal include dirs to `cap/wasm.c`.
`WASMMemoryInstance`'s layout is config-dependent (`DefPointer` padding under JIT, etc.),
so any drift between Hull's macro set and `WAMR_CFLAGS` silently reads a wrong offset.
That coupling is a permanent maintenance hazard.

Chosen: **WAMR patch `0005` adds a public accessor.** Hull TUs keep zero WAMR-internal
includes and zero WAMR config macros.

### Accessor contract

```c
/* wasm_export.h (public API) */
WASM_RUNTIME_API_EXTERN bool
wasm_runtime_memory_is_memory64(const wasm_memory_inst_t memory_inst);
```

- **NULL** `memory_inst` → returns `false` (fail-closed; matches the null tolerance of
  the sibling public getters `wasm_memory_get_shared` / `_get_cur_page_count`).
- **wasm32 module** → `false`. The `is_memory64` field exists unconditionally in
  `WASMMemoryInstance` (not under any `#if`) and is 0 for wasm32, so the accessor is
  well-defined and linkable **regardless of whether WAMR was built with Memory64** —
  it just always returns `false` in a hypothetical non-Memory64 WAMR build.
- **Memory64 module** → `true`.
- Total function, no allocation, no lock; a pure field read behind the ABI.

### Implementation (under WAMR's own config)

Body lives in `core/iwasm/common/wasm_memory.c`, compiled with `WAMR_CFLAGS`
(`WASM_ENABLE_MEMORY64=1` + the full internal include set), so it reads
`((WASMMemoryInstance *)memory_inst)->is_memory64` with the **same struct layout the
rest of WAMR was compiled against**. Correctness of the layout is WAMR's own
invariant, not something Hull re-derives.

### Detection rewire (`cap/wasm.c`)

Delete the `#if WASM_ENABLE_MEMORY64` block and the `../interpreter/wasm_runtime.h`
include. Replace with the public two-liner (no gate):

```c
wasm_memory_inst_t mem = wasm_runtime_get_default_memory(tmp_inst); /* public */
if (mem && wasm_runtime_memory_is_memory64(mem))                    /* public (0005) */
    detected_memory64 = 1;
```

`mod->is_memory64` then drives the existing plain-C paths unchanged.

## D2 (LOCKED) — patch ordering + checksum maintenance

- `patches/wamr/0005-memory64-public-accessor.patch`, applied **after** 0004.
- Touches exactly two files: `core/iwasm/include/wasm_export.h` (declaration) and
  `core/iwasm/common/wasm_memory.c` (definition). Both are already modified by
  0003/0004, so 0005 is authored **against the 0001–0004-applied tree** (`git diff`
  after those apply) so its hunk context matches; it must not perturb 0003/0004 hunks.
- `scripts/wamr_apply_patches.sh`: add `P5=…0005-…patch` and `SHA5=<sha256>`, extend
  the apply sequence and the recorded-hash gate. The script's "any changed/new file
  NOT declared by the patches → fail" guard means 0005's declared file set must be
  exactly those two files. `--dry-run` (the CI gate) must stay green.
- Base commit pin (`c3a78cd…`) is unchanged; 0005 is additive.

## D3 (LOCKED) — fixture + wamrc requirement

- Fixture: the committed `(memory i64)` module `tests/fixtures/compute/echo64.wasm`
  (echoes input to output) exporting `hull_process` with the `(i64,i64,i64,i64)->i32`
  ABI and `hull_version`.
- **Memory64 requires AOT** (fast interp cannot load it), so the fixture is embedded as
  an `.aot`. **There is NO `--enable-memory64` flag** in the vendored `wamrc` — it
  **auto-detects** Memory64 from the module's `(memory i64)` type (passing the flag
  prints usage and fails). A dedicated `GEN_MEM64_AOT` macro (`mk/tests.mk`) drives
  the build `wamrc` with just `--opt-level=3 --bounds-checks=1` and surfaces its
  stderr on failure. **Do NOT pass `--enable-shared-heap`** for the mem64 fixture: a
  shared-heap-codegen AOT run with no heap attached **segfaults** on a Memory64
  module (confirmed in CI — the dispatch crashed with the flag, passed once it was
  dropped; wasm32 AOTs are unaffected), and echo64 uses no shared heap.
- The `.wasm` is retained only to prove the interp-rejection leg (D4.2).

## D4 (LOCKED) — non-skippable CI legs

Each mirrors the existing span "must NOT skip" gate in `.github/workflows/ci.yml`
(fail the job if the wamrc-unavailable skip path is taken):

1. **Detection** — load the Memory64 module as the **interpreter** `echo64.wasm`
   (detection reads the flag at load, no AOT needed) → `is_memory64 == 1`; load a
   wasm32 module → `is_memory64 == 0` (through the same accessor). Runs unconditionally
   (no wamrc dependency); only legs 3–4 need the AOT fixture.
2. **AOT enforcement** — present a Memory64 module to the interpreter path →
   `memory64_requires_aot`, clean rejection, no dispatch.
3. **8-cell dispatch + readback** — call `hull_process` on the Memory64 AOT fixture with
   a known input; assert the output bytes. This is the first real exercise of the
   `(i64,i64,i64,i64)` marshalling (argv[0..7]) and the return readback.
4. **Memory64 SPAN_INFO** (the deferred mapped-spans 3a leg, #313). Two distinct
   address concepts, tested separately:
   - **Metadata scratch destination** (`ptr`, the 2nd host_call arg): **necessarily
     below 4 GiB because `ptr` is `i32`.** A record written to a scratch below 4 GiB
     → `count`, record fields, and the write all succeed; an invalid unsigned-i32
     destination (`0xfffffff0`) → `-1` with the invocation un-poisoned. A scratch
     destination genuinely above `UINT32_MAX` is **unrepresentable** under the fixed
     `(i32,i32,i32)->i32` host_call ABI and is therefore a **PERMANENT NON-GOAL**,
     not a deferral — it can only change if that host_call ABI changes.
   - **Returned span `base`** (the window's guest address, in the record): **64-bit.**
     Under Memory64 it must be exercised **above `UINT32_MAX`** — a span whose window
     sits in the high 64-bit space, read back correctly through the record's 64-bit
     `base`.

   **DONE via [#334](https://github.com/artalis-io/hull/issues/334)** (D4.1–D4.3
   landed here in #318; D4.4 landed in #334). #334 added a hand-authored `(memory i64)`
   span-reading fixture (`spanread64`) and the CI-gated `memory64_span_readback` test:
   a span window placed above `UINT32_MAX` is read back through the record's 64-bit
   `base` under mem64 AOT — confirming WAMR's guarded-subrange RO shared-heap
   addressing is memory64-correct under AOT. The scratch-below-4-GiB vs 64-bit-`base`
   distinction was carried verbatim into #334. The `hull build` AOT path for a
   Memory64 compute *plugin* (`build.lua`'s bogus `--enable-memory64` + the
   shared-heap codegen policy) was SPLIT to
   [#336](https://github.com/artalis-io/hull/issues/336) and is NOT yet supported;
   #318/#334 prove the runtime dispatch + mapped spans by loading a hand-compiled
   `.aot` directly via `hl_cap_wasm_load`, bypassing `build.lua`.

Convert `test_wasm.c::memory64_detection` and `memory64_rejects_interpreter` from
their `#if WASM_ENABLE_MEMORY64` `#else` (disabled) form to the live assertions above;
the `#if` gate disappears (detection no longer depends on the macro in the test TU).

## D5 (LOCKED) — wasm32 regression + docs

- **wasm32 unchanged:** the whole existing WASM suite stays green (a wasm32 module takes
  the byte-for-byte same `is_memory64 == 0` path). Add one explicit assertion that a
  wasm32 module reports `false` through the new accessor, so a future accessor
  regression is caught, not just inferred.
- **Docs:** once landed, correct the statements that Memory64 "ships" /
  "dispatches the correct calling convention" and remove the "compiled out" caveat in:
  `CLAUDE.md` (WASM section — currently carries the #318 caveat), `docs/wamr_architecture.md`,
  and `docs/roadmap.md` ("Done"). Point them at this record.

## Non-goals

- **Above-`UINT32_MAX` metadata-scratch destination — PERMANENT.** The host_call
  `ptr` arg is `i32`; such a destination is unrepresentable and stays a non-goal
  unless the `(i32,i32,i32)->i32` host_call ABI itself changes. (Distinct from the
  returned span `base`, which is 64-bit and IS exercised above `UINT32_MAX` — D4.4.)
- Interp Memory64 (WAMR does not support it).
- Any change to the wasm32 path.
- Broad Memory64 feature surface beyond the `hull_process` compute ABI already written.
