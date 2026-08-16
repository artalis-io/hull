#!/bin/sh
# tests/bench_mapped_span.sh -- controlled runner + gates for the mapped-span
# performance benchmark (docs/mapped_span_benchmark_design.md). This is a
# correctness/enforcement harness, NOT a pass/fail perf gate: it publishes the
# measurements and asserts the methodology invariants that MUST hold before any
# number is trusted. The ≤10-15% steady-state comparison is REPORTED (a warning
# if exceeded), not gated, until per-arch baselines are stable (D11).
#
# Gates (fail the build):
#   G1  drift: the committed bench_span_guest.wasm matches a fresh rebuild
#       (SDK header + guest source honesty), when clang(wasm32) is available.
#   G2  must-not-skip: with wamrc present, every workload has a representable
#       hullspan_aot row (representable==1). A skipped wasm path fails here.
#   G3  correctness: the bench itself exits 0 (its in-process gate already
#       rejects any wasm checksum that != native, and any nonzero per-scan
#       host-call delta, BEFORE trusting timing).
#   G4  D9 bytecode: wasm-objdump shows host_call ONLY in setup -- the module
#       has exactly the 2 hull_span_setup call sites, none in the scan loop.
#       (The authoritative runtime proof is G3's hostcall_scan_delta==0 check
#       inside the bench; this is the static companion. wasm-objdump optional.)
#
# Env: DATASET_MB (default 96), ITERS, WARMUPS, REPS_K, CACHE=warm|cold,
#      OVERHEAD_WARN_PCT (default 15). Usage: sh tests/bench_mapped_span.sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BW="bench/wasm"
OUT="build/bench_mapped_span.json"
: "${DATASET_MB:=96}"
: "${OVERHEAD_WARN_PCT:=15}"
export DATASET_MB OUT

fail() { echo "bench-mapped-span: FAIL: $*" >&2; exit 1; }

# ── G1 drift guard ──────────────────────────────────────────────────────────
if command -v clang >/dev/null 2>&1 && clang --print-targets 2>/dev/null | grep -q wasm32; then
    cp "$BW/bench_span_guest.wasm" build/bench_span_guest.committed.wasm
    sh "$BW/build_bench_span_guest.sh" >/dev/null
    if ! cmp -s "$BW/bench_span_guest.wasm" build/bench_span_guest.committed.wasm; then
        cp build/bench_span_guest.committed.wasm "$BW/bench_span_guest.wasm"  # restore
        fail "G1 drift: committed bench_span_guest.wasm != rebuild from source+SDK"
    fi
    echo "bench-mapped-span: G1 OK (guest wasm reproduces from source)"
else
    echo "bench-mapped-span: G1 SKIP (no clang wasm32 -- committed .wasm used as-is)"
fi

# ── build + run ─────────────────────────────────────────────────────────────
make bench-mapped-span

# ── G3 correctness: the bench exited 0 above (set -e). Re-affirm from JSON. ──
[ -f "$OUT" ] || fail "no JSON at $OUT"
if grep -q '"impl": "hullspan_aot", "representable": -3' "$OUT"; then
    fail "G2 must-not-skip: a hullspan_aot row is aot-absent (wamrc missing?)"
fi
# every hullspan_aot row must be representable==1
rep_ok=$(grep -c '"impl": "hullspan_aot", "representable": 1' "$OUT" || true)
[ "$rep_ok" -ge 4 ] || fail "G2 must-not-skip: only $rep_ok/4 representable hullspan_aot rows"
echo "bench-mapped-span: G2 OK ($rep_ok/4 workloads ran AOT + span)"
echo "bench-mapped-span: G3 OK (bench exited 0 -- checksum+host-call in-process gate passed)"

# ── G4 D9 static bytecode inspection (companion to the runtime counter) ─────
if command -v wasm-objdump >/dev/null 2>&1; then
    n=$(wasm-objdump -d "$BW/bench_span_guest.wasm" 2>/dev/null | grep -c 'call 0 <host_call>' || true)
    # 2 = the two hull_span_setup queries (count + metadata), both pre-loop.
    if [ "$n" != "2" ]; then
        fail "G4 D9: expected 2 host_call sites (setup only), found $n -- a scan-loop host_call?"
    fi
    echo "bench-mapped-span: G4 OK (WASM bytecode: host_call only in setup, 2 sites)"
else
    echo "bench-mapped-span: G4 SKIP (no wasm-objdump; runtime hostcall_scan_delta==0 still enforced by G3)"
fi

# ── report the steady-state overhead (NOT a gate yet, per D11) ──────────────
echo "bench-mapped-span: measurements at $OUT (steady-state overhead is reported, not gated)"
echo "bench-mapped-span: DONE"
