#!/bin/sh
# Keel flag-propagation gate.
#
# WHY THIS EXISTS. Hull passes KEEL_EXTRA_CFLAGS / KEEL_EXTRA_LDFLAGS to Keel's
# sub-make, and for a long time Keel referenced NEITHER name, so Hull's LTO and
# CFI flags reached no Keel TU on any platform. HL_OPT was worse: it was not
# passed at all, so Keel always built at its own hardcoded -O2 - which wedged
# the Windows source build, because cosmocc's gcc hangs on large TUs at -O2.
# See hull#461 and artalis-io/keel#261.
#
# None of that was visible: a flag that is passed and silently discarded looks
# exactly like a flag that works. Nothing failed, nothing warned. So the check
# has to assert on the ACTUAL compiler command lines Keel ends up running, not
# on the sub-make invocation Hull writes, and certainly not on the knob names.
#
# HOW. `make -n` through Hull's own recipe, then read Keel's inner compile
# lines. Dry run, no artifacts, ~seconds - cheap enough to sit in the lint job.
#
# Keel's TUs are identified by their -o target. Keel v3.0.0 moved objects under
# `build/<backend>/` so switching BACKEND= cannot mix incompatible objects into
# one archive, making the target `build/<backend>/src/*.o` and
# `build/<backend>/vendor/llhttp/*.o` relative to vendor/keel; the pre-v3.0.0
# `src/*.o` / `vendor/llhttp/*.o` shape is still accepted so this gate keeps
# working across a bisect. Hull compiles everything FLAT to `build/*.o`, so the
# extra directory level is what tells the two apart - matching `build/` alone
# would sweep in every Hull TU and make the assertions meaningless.
# Both groups are checked separately and deliberately: Keel
# splits CFLAGS (core) from VENDOR_CFLAGS (llhttp, the miniz adapter), and the
# vendored half is exactly where the Windows wedge was observed, so a hook that
# reached only one of them would look fine here and still be broken.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

MAKE="${MAKE:-make}"
FAILED=0

pass() { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; FAILED=$((FAILED + 1)); }

if [ ! -f vendor/keel/Makefile ]; then
    echo "check-keel-flags: vendor/keel is not checked out (git submodule update --init)" >&2
    exit 2
fi

# Keel's compile lines for one configuration. $1 = extra make variables.
#
# -B (always-make) is load-bearing, not belt and braces. Without it the gate
# only works on a tree where vendor/keel/libkeel.a has never been built: the
# archive's only prerequisites are Hull's mbedTLS objects, so once it exists
# make answers "'vendor/keel/libkeel.a' is up to date", never enters the
# sub-make, and emits no compile lines at all. The gate then reported six
# failures - "no Keel compile lines found (recipe changed?)" - on every
# developer machine that had built Hull, which reads as a regression in the
# thing being gated rather than as the gate mis-firing. -B makes it ask what
# the commands WOULD be, which is the actual question.
keel_lines() {
    # shellcheck disable=SC2086
    $MAKE -Bn $1 vendor/keel/libkeel.a 2>/dev/null | grep -- ' -c -o ' || true
}

core_flags()   { printf '%s\n' "$1" | grep -E -- ' -c -o (build/[^/]+/)?src/'; }
vendor_flags() { printf '%s\n' "$1" | grep -E -- ' -c -o (build/[^/]+/)?vendor/llhttp/'; }

# $1 label, $2 make vars, $3 pattern that must appear, $4 pattern that must NOT
# appear (empty to skip the negative assertion).
check_config() {
    label=$1; vars=$2; want=$3; unwanted=$4
    out=$(keel_lines "$vars")

    if [ -z "$out" ]; then
        bad "$label: no Keel compile lines found at all (recipe changed?)"
        return
    fi

    for half in core vendor; do
        case $half in
            core)   lines=$(core_flags "$out" || true) ;;
            vendor) lines=$(vendor_flags "$out" || true) ;;
        esac

        if [ -z "$lines" ]; then
            bad "$label: no Keel $half TUs in the dry run"
            continue
        fi

        # Every line in the half must carry the wanted flag: a partial
        # application is the failure mode this gate exists to catch.
        total=$(printf '%s\n' "$lines" | wc -l | tr -d ' ')
        hits=$(printf '%s\n' "$lines" | grep -c -- "$want" || true)
        if [ "$hits" = "$total" ]; then
            pass "$label: $half ($total TUs) carry $want"
        else
            bad "$label: $half only $hits/$total TUs carry $want"
            printf '        first offending line:\n        %s\n' \
                "$(printf '%s\n' "$lines" | grep -v -- "$want" | head -1 | cut -c1-160)"
        fi

        if [ -n "$unwanted" ]; then
            if printf '%s\n' "$lines" | grep -q -- "$unwanted"; then
                bad "$label: $half unexpectedly carries $unwanted"
            else
                pass "$label: $half free of $unwanted"
            fi
        fi
    done
}

echo "check-keel-flags: asserting Hull's flags reach Keel's compiler command lines"

# Every configuration states ALL THREE knobs explicitly, so the gate is
# hermetic. Left implicit, an inherited HL_ENABLE_LTO=1 (exported in a shell,
# or set by a wrapping build) silently turns the "default" case into an LTO
# build, and the gate then reports a failure that says nothing about
# propagation. A make command-line assignment beats the environment, which is
# what makes this airtight.

# 1. Default. Guards the other direction: this gate must not quietly turn every
#    build into something other than what it was.
check_config "default"          "HL_OPT=-O2 HL_ENABLE_LTO=0 HL_ENABLE_CFI=0" ' -O2 '  ' -flto'

# 2. The defect #461 is about. Before the fix this was -O2 in both halves.
check_config "HL_OPT=-O0"       "HL_OPT=-O0 HL_ENABLE_LTO=0 HL_ENABLE_CFI=0" ' -O0 '  ' -flto'

# 3. LTO, only where the toolchain can actually do it - same reasoning as the
#    CFI case below. Hull's HL_LTO_CFLAG comes from a probe (-flto=thin, then
#    -flto) and stays EMPTY when the compiler can do neither, so on such a
#    toolchain there is no flag to propagate and asserting one tests the probe
#    rather than propagation. Mirror the probe, not the flag name.
lto_probe=no
for lto_f in -flto=thin -flto; do
    if printf 'int main(void){return 0;}\n' | \
       ${CC:-cc} -Werror "$lto_f" -x c -c -o /dev/null - 2>/dev/null; then
        lto_probe=yes
        break
    fi
done
if [ "$lto_probe" = yes ]; then
    check_config "HL_ENABLE_LTO=1"  "HL_OPT=-O2 HL_ENABLE_LTO=1 HL_ENABLE_CFI=0" ' -flto' ''
else
    printf '  skip  LTO: %s cannot do -flto (no flag to propagate)\n' "${CC:-cc}"
fi

# 4. Both together, since they travel by different mechanisms (KEEL_OPT vs
#    KEEL_EXTRA_CFLAGS) and could regress independently.
check_config "HL_OPT=-O0 + LTO" "HL_OPT=-O0 HL_ENABLE_LTO=1 HL_ENABLE_CFI=0" ' -O0 '  ''

# 5. CFI, only where the toolchain can actually do it. Hull's own probe refuses
#    -fsanitize=cfi-icall on anything but clang, and degrades to LTO-only with a
#    warning; asserting CFI flags on gcc would test the probe, not propagation.
cfi_probe=$(printf 'int main(void){return 0;}\n' | \
    ${CC:-cc} -Werror -flto -fsanitize=cfi-icall -x c -c -o /dev/null - 2>/dev/null && echo yes || echo no)
if [ "$cfi_probe" = yes ]; then
    check_config "HL_ENABLE_CFI=1" "HL_OPT=-O2 HL_ENABLE_CFI=1" ' -fsanitize=cfi-icall' ''
    check_config "HL_ENABLE_CFI=1" "HL_OPT=-O2 HL_ENABLE_CFI=1" ' -fsplit-lto-unit' ''
else
    printf '  skip  CFI: %s cannot do -fsanitize=cfi-icall (Linux clang only)\n' "${CC:-cc}"
fi

if [ "$FAILED" -eq 0 ]; then
    echo "check-keel-flags: OK (Hull's flags reach Keel's core AND vendored TUs)"
    exit 0
fi
echo "check-keel-flags: $FAILED failure(s)" >&2
exit 1
