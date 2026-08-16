# Mapped-span performance benchmark — decision record

**Status:** DESIGN, pre-implementation. Closes the one unmet acceptance criterion
of the mapped-spans arc (#309/#313/#324/#325/#334/#336): the feature's defining
claim — a mapped `HullSpan` read approaches native `mmap` performance — was never
**measured**. The original spec is explicit: *"Do not claim success merely because
copies were eliminated. Measure actual generated-code performance… mapped HullSpan
sequential scan ≤ 10–15% overhead vs native mmap."* This record locks the
methodology; nothing else (no fuzzer, no buffer-ownership refactor).

## 0. What exists to build on (no parallel harness)

- `bench/wasm/bench_wasm.c` (`BENCH_WASM_SRCS`, `make bench-wasm`) — the compute
  benchmark harness (native/interp/AOT). Same link surface + build pattern the new
  bench reuses; it does NOT benchmark spans.
- `tests/hull/cap/test_span_diff.c` — the **equivalence backbone**: one shared ops
  body compiled BOTH natively (this TU, `-Itemplates`) and as a real wasm guest
  (`tests/fixtures/compute/spandiff.c`, same `*_ops.h`). The bench uses the same
  "one source, two targets" trick so native and wasm run byte-identical logic.
- The span-attach-from-C path: `hl_cap_wasm_call(cache, name, in, in_len, &out,
  &out_len, &opts, …)` with `opts.spans = &(HlWasmSpanReq){name, buf}` and `buf =
  hl_cap_fs_mmap_window(&cfg, path, off, len, …)`. This is how the harness drives
  the HullSpan read.
- `wasm-objdump -j Import` (wabt) — the codegen-inspection precedent
  (`e2e_compute_memops.sh` asserts a guest has NO `env.*` imports). The bench reuses
  it to prove no per-access host call in the hot loop.
- The AOT fixture pipeline (`GEN_GSUB_AOT` / `wamrc --enable-shared-heap`) — the
  bench guest is AOT-compiled the same way (must-not-skip when wamrc is present).
- No `getrusage`/RSS/page-fault helper exists anywhere — added here, bench-local.

## D1 (LOCKED) — four workloads, one shared ops body

`bench/wasm/bench_span_ops.h` — ONE header, compiled native + wasm, holds all four
workloads, each a pure function of a byte window `(w, len)` returning a `u64`
checksum. No workload calls any host import.

1. **Sequential bytes** — sum every byte (`u8`) across `len`.
2. **Sequential u32/u64** — sum little-endian `u32` then `u64` strides.
3. **Deterministic random access** — a fixed LCG walk of `N` offsets (seeded
   constant, same sequence every run/impl), each a bounds-checked read.
4. **Parser-like scan** — a length-prefixed record walk (read a `le32` length,
   skip/checksum that many bytes, repeat) — the OSM/Parquet-shaped access the
   feature exists for.

Reads go through the shipped SDK accessors (`hull_span_read_u32le(w,len,off,&v)`
etc.) so the SAME inlineable code path is exercised by every implementation.

## D2 (LOCKED) — four semantically-equivalent implementations

The workloads differ ONLY in what `w` points at (and, for chunked, an outer copy
loop). The ops body is identical → checksums identical by construction.

| Impl | `w` is | Notes |
|------|--------|-------|
| **native mmap** (baseline) | `mmap(file, PROT_READ, MAP_PRIVATE)` pointer, native C | the target to beat; no wasm |
| **HullSpan AOT** (under test) | the span window base (a wasm-domain address; WAMR shared-heap-translated) | `hl_cap_wasm_call` with `opts.spans` on the AOT guest |
| **copy-once into linear memory** (baseline) | a linear-memory buffer the host filled once from the file | needs a wasm heap ≥ dataset (part of WHY spans win) |
| **chunked-copy** (baseline) | a fixed linear-memory chunk buffer, host re-fills native→wasm per chunk | the outer copy loop is the measured cost |

Guest (`bench/wasm/bench_span_guest.c`, AOT): one `hull_process` with a workload +
mode selector in its input; `mode=span` reads via the attached span, `mode=linear`
reads the host-provided linear-memory buffer. Native baseline runs the same ops
body in-process over the mmap. Chunked drives the guest per chunk from the host.

## D3 (LOCKED) — setup vs steady-state; the target applies to steady-state

Each `(impl, workload)` reports TWO numbers, never conflated:
- **setup** — map / attach (span set init+attach) / allocate+copy (copy-once) /
  per-chunk copy amortized (chunked). Reported for all.
- **steady-state** — the scan/parse loop only, the span already attached and its
  first-touch faults already taken (see D5 warm protocol).

**The ≤10–15% threshold applies to `steady-state(HullSpan AOT)` vs
`steady-state(native mmap)`, per workload.** copy-once/chunked are context (they
show the copy cost spans avoid), not the pass/fail comparand.

## D4 (LOCKED) — identical dataset, order, checksum, byte-order, bounds

One deterministic dataset file (a fixed PRNG fill, size a CLI arg). Every impl sees
the SAME bytes, the SAME access order (the LCG/parser sequences are constants), does
the SAME little-endian decode via the SAME SDK accessors, and the SAME bounds
semantics (the SDK's `hull_span__fits` check, present in every impl). The
**correctness gate** (D8) refuses to time anything until all four checksums match.

## D5 (LOCKED) — cache-state protocols, reported not mixed

Two protocols, each run + labelled separately (never averaged together):
- **warm** — the file/window is fully pre-faulted (a throwaway read pass) before the
  timed region, so steady-state measures CPU + translation, not I/O.
- **cold** — best-effort eviction before timing: `posix_fadvise(POSIX_FADV_DONTNEED)`
  + `madvise(MADV_DONTNEED)` on the mapping (no root assumed). Because eviction
  without root is best-effort, the run REPORTS the achieved state via the measured
  **major-fault count** rather than asserting it — a cold run with ~0 major faults
  is flagged "cold not achieved," not silently trusted.

The output records which protocol each row used; the ≤10–15% headline is a **warm**
steady-state comparison (CPU/translation overhead, the thing the feature controls).

## D6 (LOCKED) — statistics, not a single timing

Per `(impl, workload, cache)`: `warmups` discarded iterations (default 3) then
`iters` timed iterations (default 15), each via `clock_gettime(CLOCK_MONOTONIC)`.
Report **median** + a dispersion measure (**MAD** and min/max), not a mean, not a
single shot. The overhead headline is `median(HullSpan)/median(mmap) - 1`.

## D7 (LOCKED) — environment + memory metrics captured

The output header records: CPU arch (`uname -m`), OS/kernel, C compiler + version,
`wamrc`/WAMR version + AOT flags (`--enable-shared-heap`, bounds-checks, opt-level),
dataset size, page size, iters/warmups. Per timed region, `getrusage(RUSAGE_SELF)`
deltas capture **minor + major page faults** and **`ru_maxrss` high-water**; a
Linux `/proc/self/statm` RSS sample brackets the run. (This also gives the
demand-paging evidence — a windowed scan of a huge sparse file shows RSS ≈ touched
pages, not logical size — though the RSS *assertion* test proper stays out of scope
here, it's a benchmark output.)

## D8 (LOCKED) — correctness gate precedes any timing

For each workload, all four implementations compute the checksum FIRST; if any
differs from the native-mmap checksum, the bench prints the mismatch and **aborts
with a non-zero exit before emitting a single timing number**. No performance claim
is ever made over semantically-divergent implementations.

## D9 (LOCKED) — generated-code inspection of the hot HullSpan loop

Two checks, both HARD (architecture invariants, not noisy perf):
- **No per-access host call:** `wasm-objdump -j Import` on the bench guest asserts
  the module imports only `env.host_call` (used once, in setup), and a
  `wasm-objdump -d` scan of the hot workload functions asserts NO `call` to the
  host_call import inside the loop body. (If wabt is absent, this sub-check skips
  with a notice; CI provides it.)
- **No accidental copy:** the HullSpan path asserts (at the C level) that no
  dataset-sized `module_malloc` / linear-memory buffer is allocated for the span
  read — the span is read in place. (copy-once/chunked DO allocate, by design.)

Best-effort AOT native-disassembly of the hot loop (objdump of the loaded `.aot`
text) is emitted to the output for human inspection but is not a gate (AOT symbol
mapping is fragile across wamrc versions).

## D10 (LOCKED) — reproducible output, not a PR comment

The bench writes a machine-readable `bench_mapped_span.json` (env header + a row per
`impl × workload × cache` with setup/steady medians, MAD, min/max, page faults,
RSS) to a stable path, uploaded as a CI artifact. A short human summary table is
printed to stdout. The JSON is the artifact of record.

## D11 (LOCKED) — CI: controlled job, publish-not-gate initially

- A dedicated **controlled benchmark job** (its own CI job, `runs-on` a fixed
  runner) builds wamrc + the AOT guest and runs the bench on a **small deterministic
  dataset** (CI-bounded, e.g. 64–128 MiB; the ≥1 GiB run is for a local/controlled
  invocation, `DATASET_MB=1024`).
- **Hard gates in CI:** (a) the correctness gate D8; (b) AOT **must-not-skip** (fail
  if the span path took the interpreter/no-wamrc path — the whole point is AOT
  codegen); (c) D9's no-host-call-in-loop inspection.
- **NOT a hard gate initially:** the ≤10–15% steady-state threshold. CI **publishes**
  the measured overhead (artifact + summary) but does not fail on it, per the
  requirement to avoid a noisy perf gate before per-architecture baselines are
  stable. Promotion to a regression gate is a follow-up once x86_64 + aarch64
  baselines are established (a threshold-with-margin per arch, tracked separately).

## Non-goals (this change)

- The span offset/length + window-ops **fuzzer** (separate, spec's testing section).
- The **buffer-ownership** unification (`HlMappedBuffer`/`HlWasmBuffer` → one
  `HullBuffer`) — separate architectural work.
- **Writable** spans (RO is the shipped + spec'd initial scope).
- **Memory64** whole-file benchmarking — the initial bench is wasm32-windowed
  (matches the shipped 1 GiB window cap + the "AOT on x86-64/ARM64" target); a mem64
  large-window bench is a noted follow-up.
- Promoting the perf threshold to a required gate (follow-up, after baselines).

## Deliverables

`bench/wasm/bench_span_ops.h` (shared workloads), `bench/wasm/bench_span_guest.c`
(+ committed `.wasm`, drift-guarded like `spanread64`), `bench/wasm/bench_mapped_span.c`
(harness), a `make bench-mapped-span` target, `tests/bench_mapped_span.sh` (the
controlled runner: correctness + AOT-must-not-skip + codegen inspection + JSON), and
a CI job that publishes the artifact. Docs: this record + a results section in
`docs/wamr_architecture.md` once the first numbers land.
