#!/bin/sh
# tests/check_site_consistency.sh - drift gate for the marketing site (site/).
#
# Keeps the landing page from silently going stale against a release. Three
# checks, each fatal:
#
#   1. VERSION   the "current version" the site advertises matches the latest
#                released version in CHANGELOG.md - both the JSON-LD
#                softwareVersion and every element tagged `data-hull-version`.
#   2. TABS      the install section keeps its three platform tabs
#                (data-tab="linux" / "macos" / "windows").
#   3. WINDOWS   the Windows PowerShell installer (install.ps1) is still offered.
#
# So when a release is cut, whoever bumps CHANGELOG.md must also bump the site's
# version markers, or this gate fails. Historical per-feature ship-tags (e.g.
# "Compiler-free builds (v0.10.0)") are deliberately NOT touched - only the
# `data-hull-version` / softwareVersion "current version" surface is enforced.
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

FAIL=0
err() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL + 1)); }
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }

[ -f "$SITE" ]      || { echo "check_site_consistency: missing $SITE"; exit 2; }
[ -f "$CHANGELOG" ] || { echo "check_site_consistency: missing $CHANGELOG"; exit 2; }

# Latest RELEASED version = the first versioned heading in CHANGELOG.md. The
# leading "## [Unreleased]" heading has no digits, so it is skipped.
CURRENT=$(grep -oE '^## \[[0-9]+\.[0-9]+\.[0-9]+\]' "$CHANGELOG" | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
[ -n "$CURRENT" ] || { echo "check_site_consistency: cannot parse latest version from $CHANGELOG"; exit 2; }

echo "site-consistency gate (latest release v$CURRENT):"

# ── 1. VERSION ────────────────────────────────────────────────────────────────
# 1a. JSON-LD softwareVersion (no leading v).
if grep -q "\"softwareVersion\": \"$CURRENT\"" "$SITE"; then
    ok "1/VERSION: JSON-LD softwareVersion is $CURRENT"
else
    err "1/VERSION: JSON-LD softwareVersion is not \"$CURRENT\" (bump it in $SITE)"
fi

# 1b. Every data-hull-version element must advertise v$CURRENT, and there must be
# at least MIN_MARKERS of them (so they cannot be silently deleted to pass).
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

# ── 2. TABS ───────────────────────────────────────────────────────────────────
before=$FAIL
for p in linux macos windows; do
    grep -q "data-tab=\"$p\"" "$SITE" \
        || err "2/TABS: install platform tab '$p' (data-tab=\"$p\") is missing from $SITE"
done
[ "$FAIL" -eq "$before" ] && ok "2/TABS: Linux / macOS / Windows install tabs present"

# ── 3. WINDOWS ────────────────────────────────────────────────────────────────
if grep -q 'install\.ps1' "$SITE"; then
    ok "3/WINDOWS: install.ps1 (Windows PowerShell installer) is offered"
else
    err "3/WINDOWS: $SITE does not offer install.ps1 (the Windows installer)"
fi

if [ "$FAIL" -ne 0 ]; then
    printf '\n\033[31m%d site-consistency violation(s).\033[0m\n' "$FAIL"
    printf 'On a release, bump CHANGELOG.md AND the site version markers together.\n'
    exit 1
fi
printf '\n\033[32mAll site-consistency checks passed.\033[0m\n'
exit 0
