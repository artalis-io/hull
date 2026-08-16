# SPAN_INFO under Memory64 — decision record (#334)

**Status:** DESIGN, pre-implementation. Follow-up to #318 (which shipped Memory64
detection + `memory64_requires_aot` + the 8-cell AOT dispatch). This closes the
deferred D4.4 leg: prove the mapped-span `SPAN_INFO` metadata path works on a
`(memory i64)` guest under AOT — specifically that a span whose window sits **above
`UINT32_MAX`** is read back correctly through the record's **64-bit `base`**.

Reference: `docs/memory64_dispatch_design.md` (#318), `docs/wasm_mapped_spans_checkpoint3.md`
(the D0.2 SPAN_INFO wire ABI).

## 0. What is already 64-bit-ready (so this is NOT cap-layer surgery)

The runtime side needs **no changes** — verified in the source:

- **The SPAN_INFO handler already writes a 64-bit `base`.** `host_call_handler`
  (`src/hull/cap/wasm.c`) does `store_u64le(rec + HL_SPAN_META_OFF_BASE, s->wasm_addr)`
  (offset 72, 8 bytes), and `len`/`foffset` likewise. `HlSpanMetaV1` is 96 bytes,
  little-endian, decoded by offset; `base`/`len`/`foffset` are 8-byte fields on
  BOTH wasm32 and mem64 (checkpoint-3 D0.2). The scratch `ptr` is validated as an
  **unsigned 32-bit** app offset (`(uint64_t)(uint32_t)ptr` → `wasm_runtime_validate_app_addr`),
  which is correct: the metadata scratch is ABI-bound below 4 GiB (a permanent
  non-goal above it — see §5), while the reported `base` is full 64-bit.
- **Span placement already uses the 64-bit ceiling.** `hl_wasm_span_set_init(set, is_memory64)`
  sets `addr_ceil = UINT64_MAX` for mem64 (vs `UINT32_MAX`), so
  `hl_wasm_span_set_attach` computes each span's `wasm_addr` near `UINT64_MAX` —
  i.e. **> `UINT32_MAX`**. The C-level `addr_accounting_memory64` test already
  asserts this placement; #334 proves a **guest** can actually read it.
- **The guest SDK already narrows the scratch.** `templates/hull_span.h`
  `hull_span__narrow` rejects a scratch `>= 4 GiB` before the host_call
  (`HULL_SPAN_ERR_ADDR`); unit-covered natively by #318's `test_span_sdk`.

So #334 = a `(memory i64)` fixture + a test + a CI gate + doc updates. No
`cap/wasm.c` / `cap/wasm_spans.c` change is expected.

## 1. The real point of this leg (and the primary risk)

The mapped-span window is exposed to the guest as a WAMR **guarded-subrange RO
shared heap** (patch 0004) placed at `wasm_addr`. Under wasm32 this is proven
(`aot_span_lifecycle`). Under **mem64 AOT** it is **UNPROVEN end-to-end**: the heap
is placed near `UINT64_MAX` and the guest must read it via a 64-bit `i64.load`,
which requires WAMR's shared-heap addressing to be memory64-correct in AOT codegen.

**RISK (the reason this test exists):** if WAMR's shared-heap subrange addressing
is not memory64-correct under AOT, the guest read traps or returns garbage. That is
a genuine finding — it would require a WAMR patch (a `0006`) or a scope change, NOT
a Hull cap-layer fix. Positive signals: `WASMSharedHeap` carries both
`start_off_mem64` and `start_off_mem32`; wasm32 AOT spans work. But this is exactly
what must be validated, not assumed. **Nothing describes Memory64 mapped spans as
validated until this CI leg is green** (carried over from #318).

## D1 (LOCKED) — fixture authoring: hand-authored `.wat` + `wat2wasm --enable-memory64`

- The committed `.wasm` fixtures are byte-reproducible with `wat2wasm --enable-memory64`
  (verified: recompiling `echo64.wat` reproduces the committed `echo64.wasm`
  byte-for-byte). This is the reliable, WAMR-accepted path and the echo64 precedent.
  Rejected: clang `--target=wasm64-unknown-unknown` (compiles, but experimental
  codegen with no committed-blob precedent and unverified wamrc acceptance).
- New fixture `tests/fixtures/compute/spanread64.wat` → committed `spanread64.wasm`.
  A `(memory i64 1)` module using the **raw** `host_call(0x04, …)` ABI (no
  `hull_span.h`, matching `spancount.c`), single-shot (NOT a parameterized driver —
  simpler + reliable in hand-wat). `hull_process`:
  1. `count = host_call(0x04, 0, -1)` — count query (no scratch).
  2. `reject = host_call(0x04, 0xfffffff0, 0)` — a bogus in-i32-range scratch; the
     host validates it against the 1-page mem64 linear memory → `-1`.
  3. cbSize handshake: `store16(rec+2, 96)`; `r = host_call(0x04, rec_ptr, 0)`
     (rec is in the low first page → scratch < 4 GiB, the D1-legal case).
  4. `base = i64.load(rec+72)`; `w0 = i64.load8_u(base)` — **the crux**: read
     window[0] through the 64-bit `base`.
  5. Output (12 bytes): `[count, w0, (reject==-1), base(u64 LE), (r==96)]`.
- **Both source and binary are committed, with a drift guard.** `spanread64.wat` +
  the generated `spanread64.wasm` are both committed. A `build_spanread64.sh`
  (mirroring `build_spandiff.sh`) documents the exact tool + invocation
  (`wat2wasm --enable-memory64`), prints the `wat2wasm --version`, regenerates the
  `.wasm`, and **verifies its SHA-256 against a recorded pin** — so an edit to the
  `.wat` without regenerating (or a wat2wasm-version drift) fails loudly instead of
  silently. The recorded SHA-256 lives in the script; the `.wat` header comment
  points at it. (Re-running the script is the reproducibility check.)

## D2 (LOCKED) — the >4 GiB base comes for free from the mem64 span set

The C test loads `spanread64` as an AOT mem64 module (so `is_memory64 == 1`
detected), then attaches ONE windowed span over a file with a known first byte:

```
buf = hl_cap_fs_mmap_window(&cfg, "a.bin", off, 4096, …)   // a.bin[off] known
out = call spanread64 with opts.spans = {{ name="src", buf }}
```

Because the module is mem64, `hl_wasm_span_set_init(set, 1)` places the window near
`UINT64_MAX`, so the record's `base` is **> `UINT32_MAX`** with no test scaffolding.
The C test asserts, from the 12-byte output: `count == 1`; `w0 == a.bin[off]` (the
window read through the 64-bit base is correct — the substantive proof);
`reject == 1` (the `0xfffffff0` scratch was rejected `-1`); the decoded
`base > UINT32_MAX`; `r == 96`. Mirrors #318's `call_echo_with_span` + the existing
span-call drivers.

## D3 (LOCKED) — AOT + shared-heap: required, and EXPECTED (not proven) to work when a heap is attached

#318 found that a `--enable-shared-heap` AOT run with **no heap attached** segfaults
on mem64. That crash condition does **not** apply here (the span mechanism **is** a
shared heap and **is** attached for the call), so `spanread64.aot` is compiled
**with** `--enable-shared-heap` — spans require it, exactly like the wasm32
`ro_heap`/`spandiff` AOT fixtures. Whether a `--enable-shared-heap` mem64 AOT runs
correctly **with** a heap attached is a **hypothesis this test validates, not an
established fact** — it is the same unknown as §1 (WAMR shared-heap addressing under
mem64 AOT). If it still crashes or misreads with a heap attached, that is precisely
the WAMR result §1 anticipates, recorded as a finding rather than papered over.

A dedicated `mk/tests.mk` gen rule (mirroring `GEN_GSUB_AOT`, which already adds
`--enable-shared-heap`; wamrc auto-detects mem64 — there is **no** `--enable-memory64`
flag, per #318) builds `spanread64.aot`, surfacing wamrc stderr on failure. Empty
fixture → the test skips when wamrc is absent.

## D4 (LOCKED) — test + non-skippable CI leg

- `tests/hull/cap/test_wasm_spans.c::memory64_span_readback` (AOT): the D2 flow;
  asserts count / 64-bit `base > UINT32_MAX` / `w0` correct / `0xfffffff0` rejected
  / `r == 96`. Skips cleanly when `spanread64_aot_len == 0`.
- A **must-NOT-skip** CI leg in `wasm-readonly-heap-aot` (which already builds
  wamrc), run in ISOLATION via `utest --filter='*memory64_span_readback'` (leading-only
  wildcard — a trailing `*` wrongly filters out, per #318), asserting it runs to OK
  and never the skip path. Mirrors #318's `memory64_aot_dispatch` gate exactly.
- The count-query + scratch-cbSize semantics for wasm32 stay covered by the existing
  interpreter `spanprobe` tests; #334 adds only the mem64-AOT-specific coverage.

## D5 — docs on landing

Flip `docs/memory64_dispatch_design.md` D4.4 from DEFERRED to DONE; update this
record's status to IMPLEMENTED; and correct the "Memory64 mapped spans NOT yet
validated" caveats in `CLAUDE.md`, `docs/wamr_architecture.md`, `docs/roadmap.md`,
and the mapped-spans docs — ONLY once the CI leg is green. If §1's risk
materialises (WAMR shared-heap mem64 gap), record the finding instead and keep the
caveats.

## Non-goals

- **Scratch destination above `UINT32_MAX`** — unrepresentable under the fixed
  `(i32,i32,i32)->i32` host_call ABI; a **permanent** non-goal unless that ABI
  changes (distinct from the 64-bit returned `base`, which IS exercised).
- Re-proving the wasm32 SPAN_INFO cbSize/capacity matrix (already covered).
- The `hull/wasm/span.h` SDK under a mem64 *guest* (would need clang-wasm64; the
  narrowing guard is already native-unit-covered). Out of scope; note only.

## Part B — SPLIT to [#336](https://github.com/artalis-io/hull/issues/336) (the `hull build` mem64 AOT path)

The `hull build` / `build.lua` AOT path for Memory64 compute plugins — its bogus
`--enable-memory64` flag, the `--enable-shared-heap` codegen policy for mem64, and a
production `hull build`-of-a-mem64-plugin E2E — is **out of scope for #334** and
tracked in **#336**. #334 proves the SPAN_INFO **runtime** path only and MUST NOT
imply the production build pipeline supports Memory64 plugins.

Interlock: #336's shared-heap policy decision (runtime attaches an empty heap vs
reject mem64 spans/segments vs manifest opt-in) should be made only AFTER §1's risk
is resolved here — if WAMR shared-heap under mem64 AOT does not work at all, a mem64
plugin cannot use spans/segments regardless, and #336's "reject" option becomes the
answer.
