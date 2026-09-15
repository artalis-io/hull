#!/bin/sh
# E2E test for `hull tools`.
#
# Verifies:
#   1. --help prints usage and exits 0
#   2. `tools list` (text + --json) renders correctly without network
#   3. `tools install <unknown>` rejects unknown names without any
#      network round-trip
#   4. `tools install` against a tag that doesn't exist surfaces a
#      clean error (no crash) - exercises the manifest-fetch path
#   5. `tools uninstall <unknown>` rejects unknown names
#   6. `tools uninstall <missing>` is a no-op (idempotent removal)
#   7. doctor's WASM section reflects the install state
#
# Does NOT exercise actual download + install (that would require a
# real release with wamrc assets). The download codepath shares all
# its trust + I/O with `hull update`, which is covered by
# tests/e2e_update.sh and the test_release / test_tools_install
# unit suites.
#
# Usage: sh tests/e2e_tools.sh
#        make e2e-tools
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0

# Use a per-run HOME under /tmp so we never touch the real ~/.hull.
HOME="$(mktemp -d -t hull-tools-e2e.XXXXXX)"
export HOME

# Two things this suite reads are unreliable from a POSIX shell on Windows:
#   - an APE's exit status arrives shifted left by 8 (jart/cosmopolitan#1521),
#     so every `[ "$RC" -eq 0 ]` below was passing without proving anything;
#   - a path hull PRINTS differs from the one this shell BUILT by the case of
#     the drive letter, because the MSYS shell canonicalises "/d/..." to
#     "D:/..." on the way in. Hull is correct; see tests/lib/hull_path.sh.
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL"
. "$(dirname "$0")/lib/hull_path.sh"
RC_TMP="${TMPDIR:-/tmp}/hull_tools_rc.$$"
# ONE trap: a second `trap ... EXIT` would REPLACE this one, not add to it.
trap 'rm -rf "$HOME"; rm -f "$RC_TMP"' EXIT

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

# For a needle that is a PATH. Falls back to the drive-canonical spelling only
# on an APE host, and only after the verbatim comparison has failed - so on
# Linux and macOS this is exactly assert_contains.
assert_contains_path() {
    msg="$1"; haystack="$2"; needle="$3"
    if hull_path_contains "$haystack" "$needle"; then
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $msg"
        echo "    expected (any drive-letter case): $needle"
        echo "    got:"
        echo "$haystack" | sed 's/^/      /'
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
        echo "    expected: $needle"
        echo "    got:"
        echo "$haystack" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

if [ ! -x "$HULL" ]; then
    echo "FAIL: $HULL not found - run 'make' first"
    exit 1
fi

echo "── hull --help breadcrumb to agent surface ──"
RC=$(hull_run "$RC_TMP" "$HULL" --help)
OUT=$(cat "$RC_TMP")
assert "exits 0"                            [ "$RC" -eq 0 ]
assert_contains "AI agents line"            "$OUT" "AI agents:"
assert_contains "points at orientation"     "$OUT" "task=orientation"

echo ""
echo "── hull tools --help ──"
RC=$(hull_run "$RC_TMP" "$HULL" tools --help)
OUT=$(cat "$RC_TMP")
assert "exits 0" [ "$RC" -eq 0 ]
assert_contains "shows usage"            "$OUT" "Usage: hull tools"
assert_contains "documents list"         "$OUT" "list"
assert_contains "documents install"      "$OUT" "install"
assert_contains "documents uninstall"    "$OUT" "uninstall"
assert_contains "mentions trust chain"   "$OUT" "Ed25519"

echo ""
echo "── hull tools list (empty install) ──"
RC=$(hull_run "$RC_TMP" "$HULL" tools list)
OUT=$(cat "$RC_TMP")
assert "exits 0" [ "$RC" -eq 0 ]
assert_contains "lists wamrc"            "$OUT" "wamrc"
# wamrc is not published for every platform - there is no cosmo build - so
# both the availability marker and the install hint below legitimately differ
# by host. Hardcoding the Linux answer made this suite fail on Windows for a
# correct report. Derive the expectation from hull's own JSON, which makes the
# check what it should have been: `tools list` and `agent tools` AGREEING about
# publication state, rather than a guess about which way it falls.
WAMRC_JSON=$(printf '%s' "$("$HULL" agent tools 2>&1)" \
    | sed -n 's/.*\({"name":"wamrc"[^}]*}\).*/\1/p')
case "$WAMRC_JSON" in
    *'"available_for_platform":true'*) WAMRC_PUBLISHED=1 ;;
    *)                                 WAMRC_PUBLISHED=0 ;;
esac
if [ "$WAMRC_PUBLISHED" = 1 ]; then
    assert_contains "marks wamrc available"   "$OUT" "[available]"
else
    assert_contains "marks wamrc unpublished for this platform" \
                    "$OUT" "[unavailable for "
fi

echo ""
echo "── hull tools list --json ──"
RC=$(hull_run "$RC_TMP" "$HULL" tools list --json)
OUT=$(cat "$RC_TMP")
assert "exits 0" [ "$RC" -eq 0 ]
assert_contains "tools array"            "$OUT" "\"tools\":"
assert_contains "wamrc entry"            "$OUT" "\"name\":\"wamrc\""
assert_contains "installed flag"         "$OUT" "\"installed\":"
assert_contains "available flag"         "$OUT" "\"available\":"

echo ""
echo "── hull tools install <unknown> (no network round-trip) ──"
OUT=$("$HULL" tools install does-not-exist 2>&1 || true)
assert_contains "rejects unknown name"   "$OUT" "unknown tool 'does-not-exist'"
# Crucially: must NOT have started downloading anything.
case "$OUT" in
    *"downloading"*|*"download"*|*"hull.sha256"*)
        echo "  FAIL fail-fast violated - network round-trip on unknown name"
        echo "$OUT" | sed 's/^/    /'
        FAIL=$((FAIL + 1))
        ;;
    *)
        echo "  ok  no network round-trip for unknown name"
        PASS=$((PASS + 1))
        ;;
esac

echo ""
echo "── hull tools uninstall <unknown> ──"
OUT=$("$HULL" tools uninstall does-not-exist 2>&1 || true)
assert_contains "rejects unknown name" "$OUT" "unknown tool"

echo ""
echo "── hull tools uninstall <missing> (idempotent) ──"
RC=$(hull_run "$RC_TMP" "$HULL" tools uninstall wamrc)
OUT=$(cat "$RC_TMP")
assert "exits 0 when nothing to remove" [ "$RC" -eq 0 ]
assert_contains "reports not installed"  "$OUT" "not installed"

echo ""
echo "── hull doctor wamrc state (not installed) ──"
OUT=$("$HULL" doctor 2>&1)
assert_contains "doctor mentions wamrc"             "$OUT" "wamrc"
assert_contains "doctor hints at tools install"     "$OUT" "hull tools install wamrc"

echo ""
echo "── hull doctor wamrc state (managed install present) ──"
# Plant a stub at the canonical location and re-run doctor.
mkdir -p "$HOME/.hull/tools"
printf '#!/bin/sh\necho stub-wamrc\n' > "$HOME/.hull/tools/wamrc"
chmod 0755 "$HOME/.hull/tools/wamrc"

OUT=$("$HULL" doctor 2>&1)
assert_contains_path "doctor shows stub path" "$OUT" "$HOME/.hull/tools/wamrc"
assert_contains "doctor flags managed"       "$OUT" "managed via"

OUT=$("$HULL" tools list 2>&1)
assert_contains "tools list shows installed" "$OUT" "[installed]"

echo ""
echo "── hull tools uninstall after planting stub ──"
RC=$(hull_run "$RC_TMP" "$HULL" tools uninstall wamrc)
OUT=$(cat "$RC_TMP")
assert "uninstall exits 0" [ "$RC" -eq 0 ]
assert_contains "reports uninstalled"        "$OUT" "uninstalled wamrc"
assert "file is gone" [ ! -e "$HOME/.hull/tools/wamrc" ]

echo ""
echo "── unknown verb ──"
OUT=$("$HULL" tools bogus-verb 2>&1 || true)
assert_contains "rejects unknown verb"   "$OUT" "unknown verb"

echo ""
echo "── hull agent context --list (registry discovery) ──"
RC=$(hull_run "$RC_TMP" "$HULL" agent context --list)
OUT=$(cat "$RC_TMP")
assert "exits 0"                            [ "$RC" -eq 0 ]
assert_contains "tasks array"               "$OUT" "\"tasks\""
assert_contains "orientation task"          "$OUT" "\"name\":\"orientation\""
assert_contains "quickstart-web task"       "$OUT" "\"name\":\"quickstart-web\""
assert_contains "quickstart-cli task"       "$OUT" "\"name\":\"quickstart-cli\""
assert_contains "quickstart-tui task"       "$OUT" "\"name\":\"quickstart-tui\""
assert_contains "gpu task"                  "$OUT" "\"name\":\"gpu\""
assert_contains "tools task"                "$OUT" "\"name\":\"tools\""
assert_contains "compute task (preexisting)" "$OUT" "\"name\":\"compute\""
assert_contains "levels array"              "$OUT" "\"minimal\""

echo ""
echo "── hull agent context (bare error hints at --list) ──"
OUT=$("$HULL" agent context 2>&1 || true)
assert_contains "error mentions --list"     "$OUT" "--list"

echo ""
echo "── hull agent context --task=orientation --level=minimal ──"
OUT=$("$HULL" agent context --task=orientation --level=minimal 2>&1)
assert_contains "returns content"           "$OUT" "Orientation for AI agents"
assert_contains "mentions --list path"      "$OUT" "agent context --list"

echo ""
echo "── hull agent overview <example app> ──"
# Run against examples/rest_api which has a manifest + routes + migrations.
# We need an absolute path because the agent runs from wherever this script
# was invoked, and migration auto-run can chdir under us.
EXAMPLE="$SRCDIR/examples/rest_api"
if [ -d "$EXAMPLE" ]; then
    RC=$(hull_run "$RC_TMP" "$HULL" agent overview "$EXAMPLE")
    OUT=$(cat "$RC_TMP")
    assert "exits 0"                              [ "$RC" -eq 0 ]
    assert_contains "app_dir set"                 "$OUT" "\"app_dir\":"
    assert_contains "runtime set"                 "$OUT" "\"runtime\":"
    assert_contains "routes block"                "$OUT" "\"routes\":"
    assert_contains "compute_modules block"       "$OUT" "\"compute_modules\":"
    assert_contains "modules_declared array"      "$OUT" "\"modules_declared\":"
    assert_contains "build_ready flag"            "$OUT" "\"build_ready\":"
else
    echo "  skip overview (no examples/rest_api in tree)"
fi

echo ""
echo "── hull agent tools (registry + state JSON) ──"
RC=$(hull_run "$RC_TMP" "$HULL" agent tools)
OUT=$(cat "$RC_TMP")
assert "exits 0" [ "$RC" -eq 0 ]
assert_contains "platform field"         "$OUT" "\"platform\""
assert_contains "tools array"            "$OUT" "\"tools\""
assert_contains "wamrc entry"            "$OUT" "\"name\":\"wamrc\""
assert_contains "available_for_platform" "$OUT" "\"available_for_platform\""
assert_contains "installed flag"         "$OUT" "\"installed\""

echo ""
echo "── hull agent compute (wamrc block) ──"
# Need a minimal app dir for `hull agent compute` to load context.
TMPAPP="$(mktemp -d -t hull-agent-compute.XXXXXX)"
printf 'app.manifest({modules={}})\n' > "$TMPAPP/app.lua"
RC=$(hull_run "$RC_TMP" "$HULL" agent compute "$TMPAPP")
OUT=$(cat "$RC_TMP")
assert "exits 0" [ "$RC" -eq 0 ]
assert_contains "wamrc block"            "$OUT" "\"wamrc\""
assert_contains "wamrc.installed"        "$OUT" "\"installed\""
assert_contains "wamrc.managed"          "$OUT" "\"managed\""
if [ "$WAMRC_PUBLISHED" = 1 ]; then
    assert_contains "install_hint points at the installer" \
                    "$OUT" "hull tools install wamrc"
else
    assert_contains "install_hint names the source route when unpublished" \
                    "$OUT" "make wamrc"
fi
rm -rf "$TMPAPP"

echo ""
echo "── hull agent tools after planting stub ──"
mkdir -p "$HOME/.hull/tools"
printf '#!/bin/sh\nexit 0\n' > "$HOME/.hull/tools/wamrc"
chmod 0755 "$HOME/.hull/tools/wamrc"
OUT=$("$HULL" agent tools 2>&1)
assert_contains "reports installed: true"   "$OUT" "\"installed\":true"
assert_contains_path "reports stub path"    "$OUT" "$HOME/.hull/tools/wamrc"
rm "$HOME/.hull/tools/wamrc"

echo ""
echo "── Summary ──"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
