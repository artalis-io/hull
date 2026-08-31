#!/bin/sh
# tests/check_site_consistency.sh - drift gate for the marketing site (site/).
#
# Keeps the landing page from silently going stale against a release, and keeps
# the install/verify tabs real (not just decorative buttons). Three checks, each
# fatal:
#
#   1. VERSION   the "current version" the site advertises matches the latest
#                released version in CHANGELOG.md - both the JSON-LD
#                softwareVersion and every element tagged `data-hull-version`.
#   2. TABS      the install section has three accessible platform tab PANELS
#                (Linux / macOS / Windows), each a role="tabpanel" wired to its
#                tab (aria-controls / aria-labelledby) and carrying the correct
#                platform installer URL - and NOT the other platform's.
#   3. WINDOWS   the verify block has a Windows PowerShell panel that runs
#                `hull verify-release` and does NOT hand Windows users POSIX shell
#                syntax (curl / bare VAR=... assignments).
#
# So a release bump has to update CHANGELOG.md and the site's version markers
# together, and the platform tabs cannot regress to buttons-without-content or a
# wrong-platform command. Historical per-feature ship-tags (e.g. "Compiler-free
# builds (v0.10.0)") are deliberately NOT touched - only the `data-hull-version`
# / softwareVersion "current version" surface is enforced.
#
# POSIX sh. Wired into `make lint` (target: check-site-consistency); the negative
# self-test tests/check_site_consistency_selftest.sh proves it bites.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

SITE=site/index.html
CHANGELOG=CHANGELOG.md
MIN_MARKERS=5
UNIX_URL='https://gethull.dev/install.sh'
WIN_URL='https://gethull.dev/install.ps1'

FAIL=0
err() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL + 1)); }
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }

[ -f "$SITE" ]      || { echo "check_site_consistency: missing $SITE"; exit 2; }
[ -f "$CHANGELOG" ] || { echo "check_site_consistency: missing $CHANGELOG"; exit 2; }

# The block of an element from the line carrying id="<id>" up to the next </pre>.
panel_block() { awk -v id="$1" '$0 ~ ("id=\"" id "\"") {f=1} f{print} /<\/pre>/{if(f){exit}}' "$SITE"; }

# Latest RELEASED version = the first versioned heading in CHANGELOG.md. The
# leading "## [Unreleased]" heading has no digits, so it is skipped.
CURRENT=$(grep -oE '^## \[[0-9]+\.[0-9]+\.[0-9]+\]' "$CHANGELOG" | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
[ -n "$CURRENT" ] || { echo "check_site_consistency: cannot parse latest version from $CHANGELOG"; exit 2; }

echo "site-consistency gate (latest release v$CURRENT):"

# ── 1. VERSION ────────────────────────────────────────────────────────────────
if grep -q "\"softwareVersion\": \"$CURRENT\"" "$SITE"; then
    ok "1/VERSION: JSON-LD softwareVersion is $CURRENT"
else
    err "1/VERSION: JSON-LD softwareVersion is not \"$CURRENT\" (bump it in $SITE)"
fi

markers=$(grep -c 'data-hull-version' "$SITE")
if [ "$markers" -lt "$MIN_MARKERS" ]; then
    err "1/VERSION: expected >=$MIN_MARKERS data-hull-version markers in $SITE, found $markers (markers removed?)"
else
    stale=$(grep -n 'data-hull-version' "$SITE" | grep -v "v$CURRENT" || true)
    if [ -n "$stale" ]; then
        err "1/VERSION: data-hull-version marker(s) do not read v$CURRENT:"
        printf '%s\n' "$stale" | sed 's/^/        /'
    else
        ok "1/VERSION: all $markers data-hull-version markers read v$CURRENT"
    fi
fi

# ── 2. TABS: three accessible install panels with the right installer URLs ─────
before=$FAIL
check_install_panel() {
    plat=$1; want=$2; forbid=$3
    grep -q "id=\"install-tab-$plat\"" "$SITE" \
        || { err "2/TABS: missing install tab button install-tab-$plat"; return; }
    grep -q "aria-controls=\"install-panel-$plat\"" "$SITE" \
        || err "2/TABS: install-tab-$plat has no aria-controls to install-panel-$plat"
    b=$(panel_block "install-panel-$plat")
    [ -n "$b" ] || { err "2/TABS: missing install tab panel install-panel-$plat"; return; }
    printf '%s\n' "$b" | grep -q 'role="tabpanel"' \
        || err "2/TABS: install-panel-$plat is not role=\"tabpanel\""
    printf '%s\n' "$b" | grep -q "aria-labelledby=\"install-tab-$plat\"" \
        || err "2/TABS: install-panel-$plat has no aria-labelledby to its tab"
    printf '%s\n' "$b" | grep -qF "$want" \
        || err "2/TABS: install-panel-$plat does not show its installer URL ($want)"
    if printf '%s\n' "$b" | grep -qF "$forbid"; then
        err "2/TABS: install-panel-$plat shows the wrong-platform installer ($forbid)"
    fi
}
check_install_panel linux   "$UNIX_URL" "$WIN_URL"
check_install_panel macos   "$UNIX_URL" "$WIN_URL"
check_install_panel windows "$WIN_URL"  "$UNIX_URL"
[ "$FAIL" -eq "$before" ] && ok "2/TABS: Linux/macOS/Windows install panels wired + correct installer URLs"

# ── 3. WINDOWS: the verify block gives Windows a PowerShell path, not POSIX ────
before=$FAIL
vb=$(panel_block "verify-panel-windows")
if [ -z "$vb" ]; then
    err "3/WINDOWS: missing verify-panel-windows (Windows has no PowerShell verify path)"
else
    printf '%s\n' "$vb" | grep -q 'role="tabpanel"' \
        || err "3/WINDOWS: verify-panel-windows is not role=\"tabpanel\""
    printf '%s\n' "$vb" | grep -q 'hull verify-release' \
        || err "3/WINDOWS: verify-panel-windows does not run hull verify-release"
    if printf '%s\n' "$vb" | grep -qE 'curl |[A-Z]+=https'; then
        err "3/WINDOWS: verify-panel-windows uses POSIX shell syntax (must be PowerShell)"
    fi
fi
[ "$FAIL" -eq "$before" ] && ok "3/WINDOWS: Windows verify panel is PowerShell + runs hull verify-release"

if [ "$FAIL" -ne 0 ]; then
    printf '\n\033[31m%d site-consistency violation(s).\033[0m\n' "$FAIL"
    printf 'On a release, bump CHANGELOG.md AND the site version markers together;\n'
    printf 'keep the Linux/macOS/Windows install panels + the Windows verify panel intact.\n'
    exit 1
fi
printf '\n\033[32mAll site-consistency checks passed.\033[0m\n'
exit 0
