#!/bin/sh
# e2e_manifest_extract_isolation.sh - JS manifest extraction runs in an
# isolated child process (issue #427).
#
# Reading `app.manifest({...})` out of a `.js` entry point means running the
# app's top level in a transient QuickJS runtime. Tearing that runtime down can
# SIGABRT: on an app whose top level leaves a self-referential closure cycle in
# the microtask queue, JS_FreeRuntime's cycle collector double-frees a promise
# resolver. In-process that killed `hull build` with exit 134 on ~50% of runs.
#
# The extraction now runs in a re-exec'd `hull __extract-manifest-js` child that
# writes its result to a file BEFORE tearing the runtime down, so:
#   - a teardown abort can no longer reach the parent (never exit 134), and
#   - a result produced before the abort is still usable.
#
# What this pins, in order:
#   1. the adversarial app fails CLEANLY and repeatably - never on a signal;
#   2. the child protocol is really in use (ok / none / err all round-trip);
#   3. the ordinary paths did not regress (manifest app builds, manifest-less
#      app builds).
#
# Check 2 is the one that keeps this honest. The parent falls back to
# in-process extraction when it cannot launch a child, so checks 1/3 alone
# would still pass if isolation silently stopped engaging - they would just go
# back to being flaky. Asserting the result-file protocol directly proves the
# isolated path exists and works.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL_BIN:-build/hull}"
[ -x "$HULL" ] || HULL="./build/hull"
if [ ! -x "$HULL" ]; then
    echo "SKIP: no hull binary at $HULL"
    exit 0
fi
HULL=$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")

# Canonicalize the work root: on macOS mktemp lives under /tmp -> /private/tmp
# and the seatbelt profile is given one path while fs access uses the other.
WORK=$(cd "$(mktemp -d)" && pwd -P)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $1"; exit 1; }
pass() { echo "PASS: $1"; }

# A JS-less hull has no extraction to isolate, and drops the child subcommand
# entirely. An unregistered verb falls through to the serve path rather than
# reporting itself, so probe by BEHAVIOUR: run the child on a trivial app and
# look for the result-file magic. (CI asserts this script reaches ALL PASS, so
# a skip here cannot quietly stand in for a regression.)
probe="$WORK/probe"; mkdir -p "$probe"
echo "export const x = 1;" > "$probe/app.js"
"$HULL" __extract-manifest-js "$probe/app.js" "$WORK/probe.res" >/dev/null 2>&1 || true
if [ ! -f "$WORK/probe.res" ] || ! head -c 13 "$WORK/probe.res" | grep -q 'HULLMANIFEST1'; then
    echo "SKIP: this hull has no JS manifest-extraction child (built HL_ENABLE_JS=0?)"
    exit 0
fi

# ── 1. The #427 reproducer: clean failure, never a signal death ──────────
#
# Module-local `spin` is the point: a GLOBAL-rooted rescheduler is not a
# garbage cycle and never tripped the bug (that variant is pinned separately in
# e2e_modular_resolution.sh). The module stays PENDING, so extraction correctly
# FAILS - the fix is about HOW it fails, not whether.
repro="$WORK/repro"; mkdir -p "$repro"
cat > "$repro/app.js" <<'JS'
import { app } from "hull:app";
const p = new Promise(() => {});                 // never resolves
function spin(){ Promise.resolve().then(spin); } // module-local -> closure cycle
spin();
await p;                                          // holds the module PENDING
app.manifest({ modules: [] });                    // unreachable
JS

# Ten runs: the pre-fix abort rate was ~50%, so a surviving regression has a
# ~1-in-1000 chance of slipping through. Each run is also time-boxed, since a
# hung extractor is a different regression that must not look like a pass.
N=10
i=0
while [ "$i" -lt "$N" ]; do
    i=$((i + 1))
    rm -f "$repro/out"
    "$HULL" build "$repro" -o "$repro/out" --no-verify-platform >"$WORK/r.log" 2>&1 &
    bp=$!
    n=0
    while kill -0 "$bp" 2>/dev/null; do
        n=$((n + 1))
        if [ "$n" -ge 60 ]; then
            kill -9 "$bp" 2>/dev/null || true
            wait "$bp" 2>/dev/null || true
            fail "run $i HUNG (bounded-drain or child-wait regressed)"
        fi
        sleep 1
    done
    if wait "$bp"; then rc=0; else rc=$?; fi

    # The whole point of the issue: 134 = SIGABRT reaching the parent. Any
    # >=128 status is a signal death and equally disqualifying.
    [ "$rc" -lt 128 ] || fail "run $i died on a signal (exit $rc) - teardown abort reached the parent"
    [ "$rc" -ne 0 ]   || fail "run $i unexpectedly SUCCEEDED (a PENDING module must fail extraction)"
    [ ! -f "$repro/out" ] || fail "run $i produced a binary from a failed extraction"
done
pass "self-referential-microtask app fails cleanly $N/$N times (no signal death, no binary)"

# The failure has to be legible, not a bare status.
grep -qi 'manifest' "$WORK/r.log" \
    || fail "the failure did not mention manifest extraction: $(cat "$WORK/r.log")"
pass "the clean failure names manifest extraction"

# ── 2. The child protocol round-trips (proves isolation is really engaged) ──
res="$WORK/result"

# 2a. "ok" - an app that declares a manifest.
okapp="$WORK/okapp"; mkdir -p "$okapp"
cat > "$okapp/app.js" <<'JS'
import { app } from "hull:app";
app.manifest({ modules: ["hull/json@1"] });
JS
rm -f "$res"
"$HULL" __extract-manifest-js "$okapp/app.js" "$res" >/dev/null 2>&1 \
    || fail "child returned non-zero for a valid manifest app"
[ -f "$res" ] || fail "child wrote no result file"
head -c 14 "$res" | grep -q '^HULLMANIFEST1' \
    || fail "result file lacks the HULLMANIFEST1 magic: $(head -c 40 "$res")"
head -1 "$res" | grep -q 'ok$' \
    || fail "expected an 'ok' status, got: $(head -1 "$res")"
tail -n +2 "$res" | grep -q 'hull/json@1' \
    || fail "the manifest JSON did not survive the child: $(tail -n +2 "$res")"
pass "child protocol: 'ok' carries the manifest JSON back"

# 2b. "none" - a valid app that simply declares no manifest.
noapp="$WORK/noapp"; mkdir -p "$noapp"
printf 'const x = 1;\nexport { x };\n' > "$noapp/app.js"
rm -f "$res"
"$HULL" __extract-manifest-js "$noapp/app.js" "$res" >/dev/null 2>&1 \
    || fail "child returned non-zero for a manifest-less app"
head -1 "$res" | grep -q 'none$' \
    || fail "expected a 'none' status, got: $(head -1 "$res")"
pass "child protocol: 'none' distinguishes a manifest-less app from a failure"

# 2c. "err" - a genuine extraction failure, reported not crashed.
badapp="$WORK/badapp"; mkdir -p "$badapp"
printf 'this is not ( valid javascript\n' > "$badapp/app.js"
rm -f "$res"
if "$HULL" __extract-manifest-js "$badapp/app.js" "$res" >/dev/null 2>&1; then
    fail "child returned zero for an unparseable app"
fi
head -1 "$res" | grep -q 'err$' \
    || fail "expected an 'err' status, got: $(head -1 "$res")"
[ -s "$res" ] || fail "err result carried no message"
pass "child protocol: 'err' reports a failure with a message"

# ── 3. The ordinary paths still build ────────────────────────────────────
build_ok() {
    d="$1"; what="$2"
    rm -f "$d/out"
    "$HULL" build "$d" -o "$d/out" --no-verify-platform >"$WORK/b.log" 2>&1 \
        || fail "$what failed to build: $(tail -5 "$WORK/b.log")"
    [ -f "$d/out" ] || fail "$what built but produced no binary"
}
build_ok "$okapp" "a JS app with a manifest"
pass "a JS app with a manifest still builds through the isolated child"
build_ok "$noapp" "a manifest-less JS app"
pass "a manifest-less JS app still builds through the isolated child"

echo "e2e_manifest_extract_isolation: ALL PASS"
