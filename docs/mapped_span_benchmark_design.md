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
| **copy-once into linear memory** (baseline) | a linear-memory buffer the host filled once from the file | guest heap SIZED to the configured dataset; its setup/copy cost is reported separately. If the dataset exceeds the configured linear-memory limit the row reports **"not representable within configured linear-memory limit"** — a finding (spans have no such ceiling), NOT a benchmark failure |
| **chunked-copy** (baseline) | a fixed BOUNDED linear-memory chunk buffer, host re-fills native→wasm per chunk (guest LINEAR mode, one scan per chunk) | the per-chunk copy is the setup cost; representable even far above the copy-once ceiling |

Guest (`bench/wasm/bench_span_guest.c`, AOT): one `hull_process` with a workload +
mode selector in its input; `mode=span` reads via the attached span, `mode=linear`
reads the host-provided linear-memory buffer. Native baseline runs the same ops
body in-process over the mmap. Chunked drives the guest per chunk from the host,
accumulating the per-chunk checksums.

**Chunked is measured for ALL FOUR workloads (AMENDED).** For the three streamable
workloads it re-fills a bounded chunk buffer and the guest scans it: **seq_bytes**
(any cut; byte sum is associative), **seq_words** (8-aligned cuts; the u32/u64
strides never straddle), **parser** (record-aligned cuts; a chunk holds whole
records) — sum of per-chunk `bench_run` == whole-file `bench_run`, and the
correctness gate proves it. **Random is ALSO representable**, not N/A: a bounded
reader serves the fixed-LCG offsets from a ONE-PAGE (4 KiB) cache, (re)loading the
page containing each scattered read (`chunked_random_pass`). It works — its checksum
matches native by construction (same seed/LCG/offsets/u32le assembly) — it just
THRASHES: a random walk almost never re-hits the cached page, so it copies ~4 KiB
per 4-byte read. The harness reports `chunk_loads`, `cache_hits`, and `bytes_copied`
(empirically ~512 MiB copied to read ~512 KiB at the 131072-read cap: a ~1000×
amplification), which is the useful quantitative comparison vs a span reading 4
bytes in place. The random read count is capped (`BENCH_RANDOM_MAX_READS`, applied
identically to native/span/chunked so checksums stay matched) so the reload volume
is bounded rather than terabytes. This fills the whole workload × baseline matrix.

**Cache state (AMENDED — warm-only scope).** #339 measures WARM steady-state only:
every impl is pre-faulted / warmed before timing. The earlier `cache="cold"` path
was removed because it did not actually measure cold — it faulted the mapping back
in before the timed scan and only touched the native mapping, not the span. A real
per-iteration cold protocol (evict BOTH the native and the span mapping each
iteration, with verified major-fault evidence) plus RSS/high-water validation is a
tracked follow-up, not part of this benchmark's claims.

## D3 (LOCKED) — steady-state is amortized, NOT end-to-end call time renamed

A single `hl_cap_wasm_call` includes span attach + dispatch + teardown + the scan;
calling that "steady-state" would smuggle one-time cost into the per-scan number.
So the guest (and the native baseline) run the workload **`k` times internally over
the already-attached window**, and steady-state is derived by **two-point
amortization**:

```
t(k) = fixed + k · steady            (fixed = attach + dispatch + first-touch;
steady = (t(K) - t(1)) / (K - 1)      steady = the marginal per-scan cost)
```

Measure `t(1)` and `t(K)` (K default 32) for every impl; the internal rep count is
the SAME across impls (native runs the ops body `k` times in-process). This
subtracts the fixed per-call cost exactly, leaving the pure scan.

**AMENDED (implementation) — setup-only control, not K internal reps.** WAMR meters
AOT execution against an instruction-gas limit whose hard ceiling is `INT_MAX`
(~2.1e9 instructions per call; larger requested gas is clamped). A single whole-file
scan of a 64–128 MiB dataset is already several hundred million instructions, so `K`
internal reps in ONE call (K·scan) blows the ceiling and the `t(K)` call fails with
GAS at exactly the mandated dataset size. D3 explicitly permitted the alternative it
listed — "measure an empty/setup-only control and report both" — so the harness uses
that: it measures `t(0 scans)` (the guest attaches the span / receives the copied
buffer and returns WITHOUT scanning) and `t(1 scan)`, and derives
`steady = median(t(1 scan)) - median(t(0 scans))`. Each timed call runs AT MOST ONE
scan, so it never approaches the gas ceiling at any dataset size. `t(0 scans)`
captures the fixed per-call cost (span attach + dispatch + teardown; or, for
copy-once/chunked, the linear-memory / per-chunk copy), so subtracting it leaves the
marginal scan — the same quantity the two-point sought, obtained gas-safely. The JSON
records `method: "setup-control"`, `steady_ns`, `setup_ns` (= `t(0 scans)`), and the
raw end-to-end `t(1 scan)`.

Each `(impl, workload)` therefore reports, in the JSON:
- **raw end-to-end** — `t(1)`, the full call incl. attach/dispatch (honest
  top-line), plus the measured **setup** sub-cost (map / span init+attach /
  allocate+copy / per-chunk copy).
- **steady-state** — the amortized `steady` above.

**The ≤10–15% threshold applies to `steady(HullSpan AOT)` vs `steady(native mmap)`,
per workload** — the defensible marginal comparison. Raw end-to-end and setup are
retained in the JSON but are NOT the pass/fail comparand. copy-once/chunked steady
+ setup are context (the copy cost spans avoid), not the comparand.

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

## D9 (LOCKED) — hot-loop inspection: WASM-bytecode + a runtime host-call counter (accurately labelled)

The goal is to prove the hot loop makes no per-access host call and no copy. Three
checks, at three levels of the stack, **each labelled for exactly what it proves**:

1. **WASM bytecode (HARD gate; proves the GUEST BYTECODE, not native code):**
   `wasm-objdump -j Import` asserts the guest imports only `env.host_call`, and a
   `wasm-objdump -d` scan of the hot workload functions asserts NO `call` to that
   import inside the loop body. Output label: *"WASM bytecode contains no
   per-access host_call"* — this does **not** claim anything about what WAMR AOT
   emitted. (Skips with a notice if wabt is absent; CI provides it.)
2. **Runtime host-call counter (HARD gate; proves the ACTUAL AOT EXECUTION):** a
   process-global `_Atomic uint64_t` incremented in `host_call_handler`
   (`cap/wasm.c`) — a single relaxed add on an already-slow boundary crossing,
   exposed via `hl_cap_wasm_host_call_count()`. The bench samples it around the
   timed steady-state region and asserts the delta is **0** (the SPAN_INFO calls
   happen in setup, before the region). This is the direct, execution-level proof
   that the AOT hot loop crosses the boundary zero times — stronger than the static
   bytecode scan and independent of disassembly.
3. **AOT/native disassembly (NOT a gate; emitted for humans):** a best-effort
   objdump of the loaded `.aot` text for the hot loop is written to the output,
   explicitly labelled *"native codegen, human inspection only — not asserted"*
   (AOT symbol mapping is fragile across wamrc versions). No performance or safety
   claim rests on it.

Plus **no accidental copy (HARD gate, C level):** the HullSpan path asserts no
dataset-sized `module_malloc` / linear-memory buffer is allocated for the span read
— the span is read in place. (copy-once/chunked DO allocate, by design.)

The honest summary the JSON records: bytecode-clean (check 1) + zero runtime
host-calls in the scan (check 2) together establish "no per-access host call in the
executed hot loop"; native machine-code quality is shown but not asserted (check 3).

## D10 (LOCKED) — reproducible output, not a PR comment

The bench writes a machine-readable `bench_mapped_span.json` (env header + a row per
`impl × workload × cache` with setup/steady medians, MAD, min/max, page faults,
RSS) to a stable path, uploaded as a CI artifact. A short human summary table is
printed to stdout. The JSON is the artifact of record.

## D11 (LOCKED) — CI: controlled job, publish-not-gate initially

- **Required PR job:** a dedicated controlled benchmark job builds wamrc + the AOT
  guest and runs the bench on a **64–128 MiB** deterministic dataset (CI-bounded).
- **Manually-triggered 1 GiB job (added now):** a `workflow_dispatch` job (its own
  workflow, or a gated input on this one) runs `DATASET_MB=1024` on a fixed runner.
  The ≥1 GiB result is **necessary before claiming the large-data acceptance
  criterion is measured**, but it does not burden every commit and is not scheduled
  — it is invoked on demand and its JSON artifact is the large-data evidence.
- **Hard gates (both jobs):** (a) the correctness gate D8; (b) AOT **must-not-skip**
  (fail if the span path took the interpreter/no-wamrc path — the whole point is AOT
  codegen); (c) D9 check 1 (WASM bytecode clean) + check 2 (zero runtime host-calls
  in the scan).
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
