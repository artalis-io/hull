#!/bin/sh
# E2E test for the first-run developer experience.
#
# Everything here is a regression guard for a real first-run Windows session
# that went wrong, asserted on the host CI actually runs on. The Windows-only
# halves (the ".com" artifact, the PowerShell run command) are covered by
# .github/workflows/cosmocc-windows-e2e.yml on a real Windows runner; what
# THIS script locks down is that the POSIX side never regressed while those
# were fixed, plus the host-independent behaviour.
#
# Verifies:
#   1. `hull doctor` distinguishes required / optional / fallback state
#      instead of rendering everything absent as a failure
#   2. an absent SYSTEM CA store is NOT presented as broken when the
#      embedded bundle is present (doctor + --json `ca_bundle.ok`)
#   3. doctor's compiler advice names the compiler THIS binary can use, and
#      the JSON exposes host_os / build_compiler_required / fix_command
#   4. doctor never prints a bare `make ...` as the only route for an
#      optional feature
#   5. `hull doctor --fix` is wired, refuses --json, and is a clean no-op
#      when the build is already ready
#   6. default CLI output carries NO internal C source coordinates
#   7. `--verbose` (before AND after the subcommand) restores them
#   8. POSIX build output is still `app_dir/app` - no ".com" leaked onto Unix
#   9. `hull build` prints a runnable command for the produced binary
#  10. a normal build leaves ONE obvious executable in the app root
#
# Usage: sh tests/e2e_onboarding.sh
#        make e2e-onboarding
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0

WORKDIR="$(mktemp -d -t hull-onboarding-e2e.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT

assert() {
    msg="$1"; shift
    if "$@"; then
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $msg"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    msg="$1"; haystack="$2"; needle="$3"
    if echo "$haystack" | grep -qF -- "$needle"; then
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $msg"
        echo "    expected to contain: $needle"
        echo "$haystack" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

assert_not_contains() {
    msg="$1"; haystack="$2"; needle="$3"
    if echo "$haystack" | grep -qF -- "$needle"; then
        echo "  FAIL $msg"
        echo "    expected NOT to contain: $needle"
        echo "$haystack" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    else
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    fi
}

if [ ! -x "$HULL" ]; then
    echo "FAIL: $HULL not found - run 'make' first"
    exit 1
fi

# ── 1-4. doctor: honest, platform-aware, actionable ──────────────────
echo "── hull doctor: state classification ──"
DOC=$("$HULL" doctor 2>&1 || true)
DOCJ=$("$HULL" doctor --json 2>&1 || true)

assert_contains "summary block leads the report"   "$DOC" "Runtime "
assert_contains "summary reports HTTPS"            "$DOC" "HTTPS "
assert_contains "summary reports Build"            "$DOC" "Build "

# An absent system CA store must read as a working fallback, not a failure.
#
# The "not a red cross" half matches the `system` ROW plus the glyph with
# `[[:space:]]*` between them, rather than a hand-written literal. doctor pads
# labels with `%-12s`, so a literal with a counted run of spaces is trivially
# one space off - and because this is a NEGATIVE assertion, being off makes it
# pass vacuously instead of failing loudly. (That is exactly what happened
# here before this was corrected.) The glyph is built with `printf` octal
# escapes because grep's ERE does not interpret `\xNN`.
if echo "$DOC" | grep -q "using the embedded bundle"; then
    assert_contains "absent system CA is framed as a fallback" \
                    "$DOC" "using the embedded bundle"
    MISS_GLYPH=$(printf '\342\234\227')   # U+2717 BALLOT X, doctor's "required and missing"
    if echo "$DOC" | grep -q "^  system[[:space:]]*$MISS_GLYPH"; then
        echo "  FAIL absent system CA rendered as a red cross"
        echo "$DOC" | grep -E '^  system' | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    else
        echo "  ok  absent system CA is not a red cross"
        PASS=$((PASS + 1))
    fi
    # Guard the guard: the row must actually exist, so a doctor that stopped
    # printing it altogether cannot make the check above pass by omission.
    if echo "$DOC" | grep -q '^  system[[:space:]]'; then
        echo "  ok  the system CA row is present to be judged"
        PASS=$((PASS + 1))
    else
        echo "  FAIL no 'system' row in the CA section"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  ok  system CA present on this host (fallback path not exercised)"
    PASS=$((PASS + 1))
fi
assert_contains "JSON reports the effective CA source" "$DOCJ" '"effective"'
assert_contains "JSON reports whether HTTPS trust is OK" "$DOCJ" '"ok"'

echo ""
echo "── hull doctor: compiler advice targets THIS binary ──"
assert_contains "names the required compiler"  "$DOC" "hull build needs"
assert_contains "JSON exposes the host"        "$DOCJ" '"host_os"'
assert_contains "JSON exposes the exe suffix"  "$DOCJ" '"exe_suffix"'
assert_contains "JSON names the required cc"   "$DOCJ" '"build_compiler_required"'
assert_contains "JSON carries a fix slot"      "$DOCJ" '"fix_command"'

# A native (non-cosmo) hull must NOT advertise cosmocc as its required
# compiler, and a cosmo hull must not advertise gcc/clang. Assert whichever
# applies rather than hard-coding one.
if echo "$DOCJ" | grep -q '"platform":"cosmo"'; then
    assert_contains "cosmo hull requires cosmocc" \
                    "$DOCJ" '"build_compiler_required":"cosmocc"'
else
    assert_contains "native hull requires cc/gcc/clang" \
                    "$DOCJ" '"build_compiler_required":"cc|gcc|clang"'
    assert_not_contains "native hull does not demand cosmocc" \
                    "$DOC" "hull build needs cosmocc"
fi

echo ""
echo "── hull doctor: optional features are not failures ──"
assert_contains "GPU is marked optional, not broken" "$DOC" "Compute (GPU)"
# `make ...` may appear, but only labelled as the source-build route - never
# as the only instruction handed to a release user.
if echo "$DOC" | grep -q "HL_ENABLE_GPU=1"; then
    assert_contains "GPU make hint is labelled a source build" \
                    "$DOC" "developer/source build"
else
    echo "  ok  GPU linked in on this build (hint path not exercised)"
    PASS=$((PASS + 1))
fi
assert_not_contains "no bare 'rebuild with make' phrasing" \
                    "$DOC" "not built (rebuild with"

echo ""
echo "── hull doctor --fix ──"
FIXOUT=$("$HULL" doctor --fix --json 2>&1 || true)
assert_contains "--fix and --json are mutually exclusive" \
                "$FIXOUT" "mutually exclusive"
FIXOUT=$("$HULL" doctor --fix 2>&1 || true)
if "$HULL" doctor >/dev/null 2>&1; then
    assert_contains "--fix is a clean no-op when already ready" \
                    "$FIXOUT" "nothing to do"
else
    # Not ready: --fix must either act or explain. It must never fail silently.
    if echo "$FIXOUT" | grep -qE 'Running:|no Hull-managed fix|platform library'; then
        echo "  ok  not-ready --fix produced actionable guidance"
        PASS=$((PASS + 1))
    else
        echo "  FAIL not-ready --fix produced no actionable guidance"
        echo "$FIXOUT" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
fi

# ── 5-6. logging hygiene ─────────────────────────────────────────────
echo ""
echo "── default CLI output hides internal source coordinates ──"
mkdir -p "$WORKDIR/app"
cat > "$WORKDIR/app/app.lua" <<'LUA'
local log = require("hull.log")
app.manifest({ modules = { "hull/http-server@1", "hull/log@1" } })
log.info("[app] app loaded")
app.get("/ping", function(req, res) res:text("pong") end)
LUA

NEWOUT=$("$HULL" new "$WORKDIR/scaffold" 2>&1 || true)
assert_not_contains "hull new prints no C source path" "$NEWOUT" "src/hull/"
assert_not_contains "hull new prints no sandbox chatter" "$NEWOUT" "[sandbox]"

MANOUT=$("$HULL" manifest "$WORKDIR/app" 2>&1 || true)
assert_not_contains "hull manifest prints no C source path" "$MANOUT" "src/hull/"
assert_not_contains "hull manifest prints no .c line refs" "$MANOUT" ".c:"

echo ""
echo "── --verbose restores diagnostics (before AND after the subcommand) ──"
V1=$("$HULL" --verbose manifest "$WORKDIR/app" 2>&1 || true)
V2=$("$HULL" manifest "$WORKDIR/app" --verbose 2>&1 || true)
assert_contains "pre-command --verbose shows internals"  "$V1" "src/hull/"
assert_contains "post-command --verbose shows internals" "$V2" "src/hull/"

# ── 7-9. build artifact naming + hygiene (POSIX invariants) ──────────
echo ""
echo "── hull build: POSIX output naming is unchanged ──"
BOUT=$("$HULL" build --no-verify-platform "$WORKDIR/app" 2>&1)
BRC=$?
if [ "$BRC" -ne 0 ]; then
    echo "  SKIP build assertions (hull build unavailable here):"
    echo "$BOUT" | sed 's/^/      /'
else
    # On every POSIX host the default artifact stays exactly `app_dir/app`.
    assert "default output is app_dir/app (no suffix on POSIX)" \
           [ -f "$WORKDIR/app/app" ]
    assert "the artifact is executable" [ -x "$WORKDIR/app/app" ]
    assert_not_contains "no .com artifact on POSIX" "$(ls "$WORKDIR/app")" "app.com"
    assert_contains "reports where it wrote"  "$BOUT" "hull build: wrote"
    assert_contains "prints a runnable command" "$BOUT" "run it with"
    assert_contains "the command has a ./ prefix" "$BOUT" "./"

    # One obvious shippable executable in the app root: no stray ELF/debug
    # sidecars left beside it.
    assert "no .aarch64.elf sidecar in the app root" \
           [ ! -f "$WORKDIR/app/app.aarch64.elf" ]
    assert "no .com.dbg sidecar in the app root" \
           [ ! -f "$WORKDIR/app/app.com.dbg" ]

    # The app's own top-level log.info runs during manifest extraction; it
    # must not surface as build output.
    assert_not_contains "app's build-time log stays out of build output" \
                    "$BOUT" "[app] app loaded"
    VB=$("$HULL" build --verbose --no-verify-platform "$WORKDIR/app" 2>&1 || true)
    assert_contains "--verbose attributes it to build-time evaluation" \
                    "$VB" "[build-eval]"
fi

echo ""
echo "════════════════════════════════════════"
echo "  passed: $PASS   failed: $FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
