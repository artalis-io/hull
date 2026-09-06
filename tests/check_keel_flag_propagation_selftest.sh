#!/bin/sh
# Negative self-test for the Keel flag-propagation gate.
#
# A gate that cannot fail is worse than no gate: it reads as coverage while
# providing none. That is not hypothetical here - the defect this gate exists to
# catch WAS a silent one (flags passed, flags discarded, nothing failed), so the
# gate's own ability to bite has to be demonstrated rather than assumed.
#
# Two probes, because the defect had two distinct shapes:
#
#   A  KEEL_OPT is not passed at all      -> the original hull#461 state, where
#                                            Keel always built at its own -O2.
#   B  the hook reaches CFLAGS but not     -> the ASYMMETRIC shape found in
#      VENDOR_CFLAGS                          review of keel#261, where core TUs
#                                            got the flag and vendored ones did
#                                            not. Under LTO that yields an
#                                            archive of mixed LTO/non-LTO
#                                            objects, which is quieter and worse
#                                            than losing the flag outright.
#
# Both probes mutate tracked files and are restored by the EXIT trap.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

GATE="sh tests/check_keel_flag_propagation.sh"
HULL_MK="mk/vendor/keel.mk"
KEEL_MK="vendor/keel/Makefile"
FAILED=0

pass() { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; FAILED=$((FAILED + 1)); }

cleanup() {
    git checkout -q -- "$HULL_MK" 2>/dev/null || true
    git -C vendor/keel checkout -q -- Makefile 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# 1. Baseline: the real tree must pass, or the probes below prove nothing.
if $GATE >/dev/null 2>&1; then
    pass "baseline: propagation is intact"
else
    bad "baseline: gate already failing on an unmodified tree"
    echo "check-keel-flags selftest: cannot proceed" >&2
    exit 1
fi

# 2. Probe A - stop passing KEEL_OPT, i.e. restore the original defect.
grep -v 'KEEL_OPT="$(HL_OPT)"' "$HULL_MK" > "$HULL_MK.probe" && mv "$HULL_MK.probe" "$HULL_MK"
out=$($GATE 2>&1) && rc=0 || rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'HL_OPT=-O0'; then
    pass "probe A (KEEL_OPT unpassed) -> gate BITES"
else
    bad "probe A (KEEL_OPT unpassed) -> gate did NOT bite (rc=$rc)"
fi
cleanup

# 3. Probe B - the asymmetric shape: hook reaches CFLAGS, not VENDOR_CFLAGS.
sed 's/^override VENDOR_CFLAGS += \$(KEEL_EXTRA_CFLAGS)$/# probe B: hook removed from the vendored half/' \
    "$KEEL_MK" > "$KEEL_MK.probe" && mv "$KEEL_MK.probe" "$KEEL_MK"
out=$($GATE 2>&1) && rc=0 || rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'vendor'; then
    pass "probe B (vendored half unhooked) -> gate BITES"
else
    bad "probe B (vendored half unhooked) -> gate did NOT bite (rc=$rc)"
fi
cleanup

# 4. Restored: clean again, so the probes left nothing behind.
if $GATE >/dev/null 2>&1; then
    pass "probes reverted -> gate CLEAN again"
else
    bad "probes reverted -> gate still failing (tree left dirty?)"
fi

[ "$FAILED" -eq 0 ] && { echo "check-keel-flags selftest: all negative checks pass"; exit 0; }
echo "check-keel-flags selftest: $FAILED failure(s)" >&2
exit 1
