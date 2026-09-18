# hull_nm.sh - make a symbol-ABSENCE assertion mean something.
#
# WHY THIS EXISTS. "This binary carries no mbedTLS" is checked by counting
# matches in nm's output and requiring zero. That zero has TWO causes, and they
# are indistinguishable:
#
#   1. the binary genuinely lacks the symbol   - what the test means;
#   2. nm could not READ the binary            - what it must never accept.
#
# GNU nm reports plainly that it cannot:
#
#   $ nm build/hull
#   nm: build/hull: no symbols
#
# A Cosmopolitan APE is not an ELF it can parse, so on any host where `hull
# build` produces one, every absence assertion pointed at a produced binary
# counts 0 of 0 and passes having inspected nothing. The claims at stake are
# the ones the composable base rests on - pure-compute links zero Keel and zero
# mbedTLS, a db-free app carries no SQLite, a non-SMTP app links none of the
# SMTP machinery - so a vacuous pass there retires the architecture's central
# invariant without a word.
#
# This has not bitten yet only because the four suites carrying such guards
# refuse to run on cosmo BEFORE reaching them. That is a fortunate accident of
# ordering, not a property of the guards: the moment a skip is lifted - which
# is exactly what giving the Windows job a drivable cosmocc does - they go live
# against binaries nm cannot read.
#
# So call hull_nm_readable on the binary FIRST. It converts "nm saw nothing"
# from a silent pass into a loud failure and leaves the assertions beneath it
# meaningful. Where a suite already checks that a symbol which MUST be present
# is present, that check is an equivalent canary and this is redundant.
#
# Requires the sourcing suite to define `fail`.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# hull_nm_readable BIN -> 0 when nm actually read symbols from BIN.
# Fails the suite (and returns 1) when it read none.
hull_nm_readable() {
    _hnm_tot=$(nm "$1" 2>/dev/null | wc -l | tr -d ' ')
    [ "${_hnm_tot:-0}" -gt 0 ] && return 0
    fail "nm read NO symbols from $1 - every symbol-absence check on it would pass vacuously (an APE is not readable by GNU nm)"
    return 1
}
