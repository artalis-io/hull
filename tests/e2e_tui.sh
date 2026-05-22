#!/bin/sh
# tests/e2e_tui.sh — End-to-end smoke tests for the hull.tui module.
#
# Real interactive testing requires a PTY (see test_tui_lifecycle.c).
# This script exercises the boring paths that don't need one:
#   - Lua and JS picker examples load through the resolver.
#   - The cap layer refuses cleanly when stdin/stdout aren't a tty.
#   - Manifest gating: declaring hull/tui@1 without `tui = true`
#     errors out before app.main runs.
#
# Visual / key-decoding behaviour is covered by:
#   - tests/hull/cap/test_tui_parser.c (32 unit tests)
#   - tests/hull/cap/test_tui_lifecycle.c (8 PTY-driven tests)
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

HULL_BIN="${HULL_BIN:-build/hull}"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }

# Assert that stderr contains `$3` and exit code is `$2`.
expect_failure_with() {
    label="$1"
    expected_rc="$2"
    needle="$3"
    shift 3
    out=$("$@" 2>&1 < /dev/null)
    rc=$?
    case "${out}" in
        *"${needle}"*)
            if [ "${rc}" = "${expected_rc}" ]; then
                pass "${label}"
            else
                fail "${label} (got rc=${rc}, expected ${expected_rc}; output OK)"
            fi
            ;;
        *)
            fail "${label} (output missing '${needle}'): ${out}"
            ;;
    esac
}

# ── Lua picker ─────────────────────────────────────────────────────

echo "--- tui_picker (Lua) ---"
expect_failure_with \
    "lua picker refuses without a tty" \
    "1" "not attached to a terminal" \
    "${HULL_BIN}" examples/tui_picker/app.lua

# ── JS picker ──────────────────────────────────────────────────────

echo "--- tui_picker (JS) ---"
expect_failure_with \
    "js picker refuses without a tty" \
    "1" "not attached to a terminal" \
    "${HULL_BIN}" examples/tui_picker/app.js

# ── Manifest gate ──────────────────────────────────────────────────

echo "--- manifest gating ---"

TMP=$(mktemp -d)
trap 'rm -rf "${TMP}"' EXIT

# 1. Declaring hull/tui without tui:true → resolver rejects.
cat > "${TMP}/missing_flag.lua" <<'EOF'
app.manifest({
    modules = { "hull/tui@1" },
})
app.main(function(ctx) ctx.stdout:write("never\n"); return 0 end)
EOF
expect_failure_with \
    "lua: missing tui:true is rejected" \
    "1" "requires the 'tui' capability" \
    "${HULL_BIN}" "${TMP}/missing_flag.lua"

cat > "${TMP}/missing_flag.js" <<'EOF'
import { app } from "hull:app";
app.manifest({ modules: ["hull/tui@1"] });
app.main(async (ctx) => { ctx.stdout.write("never\n"); return 0; });
EOF
expect_failure_with \
    "js: missing tui:true is rejected" \
    "1" "requires the 'tui' capability" \
    "${HULL_BIN}" "${TMP}/missing_flag.js"

# 2. Importing hull.tui without declaring it → resolver rejects.
#    The gate fires once the resolver has run, which happens between
#    load_app and app.main. The require call below sits inside main
#    so it hits the gate.
cat > "${TMP}/undeclared.lua" <<'EOF'
app.manifest({})
app.main(function(ctx)
    local tui = require("hull.tui")
    return 0
end)
EOF
expect_failure_with \
    "lua: undeclared hull.tui import is rejected" \
    "1" "not declared in app.manifest" \
    "${HULL_BIN}" "${TMP}/undeclared.lua"

# ── Interactive PTY-driven cases ──────────────────────────────────
#
# Requires build/e2e_tui_drive (built from tests/e2e_tui_drive.c when
# HL_ENABLE_TUI=1). If absent — e.g. cosmo build without forkpty —
# we skip these instead of failing.

DRIVE="${DRIVE:-build/e2e_tui_drive}"
if [ ! -x "${DRIVE}" ]; then
    echo "--- interactive (skipped) ---"
    echo "  (no PTY driver at ${DRIVE} — run \`make build/e2e_tui_drive\`)"
else
    echo "--- interactive picker (Lua) ---"

    # Enter immediately → first item ("apple")
    if "${DRIVE}" "apple" "%r" -- "${HULL_BIN}" examples/tui_picker/app.lua \
            > /dev/null 2>&1; then
        pass "lua: enter on first frame picks first item"
    else
        fail "lua: enter on first frame picks first item"
    fi

    # Down once + enter → second item ("apricot")
    if "${DRIVE}" "apricot" "%d%r" -- "${HULL_BIN}" examples/tui_picker/app.lua \
            > /dev/null 2>&1; then
        pass "lua: down arrow + enter picks second item"
    else
        fail "lua: down arrow + enter picks second item"
    fi

    # Four downs + enter → "cantaloupe"
    if "${DRIVE}" "cantaloupe" "%d%d%d%d%r" -- "${HULL_BIN}" examples/tui_picker/app.lua \
            > /dev/null 2>&1; then
        pass "lua: four downs + enter picks fifth item"
    else
        fail "lua: four downs + enter picks fifth item"
    fi

    # q → abort (exit code 130; we just check the "aborted" message)
    if "${DRIVE}" "aborted" "%q" -- "${HULL_BIN}" examples/tui_picker/app.lua \
            > /dev/null 2>&1; then
        pass "lua: q aborts the picker"
    else
        fail "lua: q aborts the picker"
    fi

    # Escape → abort
    if "${DRIVE}" "aborted" "%e" -- "${HULL_BIN}" examples/tui_picker/app.lua \
            > /dev/null 2>&1; then
        pass "lua: escape aborts the picker"
    else
        fail "lua: escape aborts the picker"
    fi

    echo "--- interactive picker (JS) ---"

    if "${DRIVE}" "apple" "%r" -- "${HULL_BIN}" examples/tui_picker/app.js \
            > /dev/null 2>&1; then
        pass "js: enter on first frame picks first item"
    else
        fail "js: enter on first frame picks first item"
    fi

    if "${DRIVE}" "cantaloupe" "%d%d%d%d%r" -- "${HULL_BIN}" examples/tui_picker/app.js \
            > /dev/null 2>&1; then
        pass "js: four downs + enter picks fifth item"
    else
        fail "js: four downs + enter picks fifth item"
    fi

    # ── Async-integrated poll ──────────────────────────────────
    #
    # Proves that tui.poll yields to the event loop: a background
    # coroutine spawned via tui.async ticks a counter every 50ms
    # while the main coroutine sits in tui.poll(-1). After ~500ms
    # we send Enter; the counter should be in the high single
    # digits or low teens. We assert a conservative lower bound
    # (>= 5) so the test is robust against CI jitter.
    check_async_proof() {
        runtime="$1"; ext="$2"
        OUT=$("${DRIVE}" "async_proof:" "%s500%r" -- \
              "${HULL_BIN}" "examples/tui_picker/async_proof.${ext}" 2>/dev/null \
              | grep "async_proof:" | tail -1)
        case "${OUT}" in
            *"counter=0 "*|*"counter=0\""*)
                fail "${runtime}: tui.poll yields to event loop (counter stuck at 0; output: '${OUT}')"
                ;;
            *"counter="*" key="*)
                n=$(printf "%s" "${OUT}" | sed -n 's/.*counter=\([0-9]*\).*/\1/p')
                if [ -n "${n}" ] && [ "${n}" -ge 5 ]; then
                    pass "${runtime}: tui.poll yields to event loop (bg counter=${n} after 500ms)"
                else
                    fail "${runtime}: tui.poll yields to event loop (counter=${n}, expected >= 5)"
                fi
                ;;
            *)
                fail "${runtime}: async_proof.${ext} produced no recognizable output: '${OUT}'"
                ;;
        esac
    }

    echo "--- async tui.poll yields to event loop ---"
    check_async_proof "lua" "lua"
    check_async_proof "js"  "js"

    # ── hull doctor --tui (Phase 2 dogfood) ────────────────────────
    #
    # Drives the C dispatcher → hull_tool → stdlib/lua/hull/doctor_tui
    # → hull.tui rendering chain end-to-end. The driver wakes up after
    # the initial frame is drawn, sends 'q' to quit, and asserts that
    # the rendered output contains "hull doctor" (the title bar) and
    # "Subsystems" (one of the section headers).
    echo "--- hull doctor --tui ---"
    OUT=$("${DRIVE}" "Subsystems" "%q" -- "${HULL_BIN}" doctor --tui 2>/dev/null)
    case "${OUT}" in
        *"hull doctor"*"Subsystems"*)
            pass "hull doctor --tui renders title + sections"
            ;;
        *)
            fail "hull doctor --tui missing expected sections"
            ;;
    esac

    # Non-tty path: should print a helpful message and exit non-zero.
    if "${HULL_BIN}" doctor --tui < /dev/null > /dev/null 2>&1; then
        fail "hull doctor --tui without a tty should exit non-zero"
    else
        pass "hull doctor --tui without a tty exits with a helpful error"
    fi

    # ── hull agent context --interactive (Phase 3 dogfood) ─────────
    echo "--- hull agent context --interactive ---"
    OUT=$("${DRIVE}" "hull agent context" "%q" -- \
          "${HULL_BIN}" agent context --interactive 2>/dev/null)
    case "${OUT}" in
        *"hull agent context"*)
            pass "hull agent context --interactive renders title"
            ;;
        *)
            fail "hull agent context --interactive missing title"
            ;;
    esac

    # Down + enter should print the chosen context to stdout. After
    # entering+exiting the TUI the alt-screen leaves and the raw JSON
    # is on stdout — we check for a known token from the context payload.
    OUT=$("${DRIVE}" '"task":' "%d%r" -- \
          "${HULL_BIN}" agent context --interactive 2>/dev/null)
    case "${OUT}" in
        *'"task":'*)
            pass "hull agent context --interactive prints chosen task on enter"
            ;;
        *)
            fail "hull agent context --interactive did not print on enter"
            ;;
    esac

    # ── hull agent errors --tui ───────────────────────────────────
    echo "--- hull agent errors --tui ---"
    OUT=$("${DRIVE}" "hull agent errors" "%q" -- \
          "${HULL_BIN}" agent errors --tui 2>/dev/null)
    case "${OUT}" in
        *"hull agent errors"*"No errors."*)
            pass "hull agent errors --tui renders empty-state"
            ;;
        *"hull agent errors"*)
            pass "hull agent errors --tui renders title"
            ;;
        *)
            fail "hull agent errors --tui missing expected output"
            ;;
    esac

    # ENOTTY rejection paths for both.
    if "${HULL_BIN}" agent context --interactive < /dev/null > /dev/null 2>&1; then
        fail "hull agent context --interactive without a tty should exit non-zero"
    else
        pass "hull agent context --interactive without a tty exits with a helpful error"
    fi
    if "${HULL_BIN}" agent errors --tui < /dev/null > /dev/null 2>&1; then
        fail "hull agent errors --tui without a tty should exit non-zero"
    else
        pass "hull agent errors --tui without a tty exits with a helpful error"
    fi

    # ── hull dev --tui (Phase 3 headline) ──────────────────────────
    echo "--- hull dev --tui ---"

    # Spin up a tiny test app that logs every 100ms.
    DEV_TMP=$(mktemp -d 2>/dev/null || mktemp -d -t hulldev)
    trap 'rm -rf "${DEV_TMP}"' EXIT
    cat > "${DEV_TMP}/app.lua" <<'APP'
app.manifest({})
app.main(function(ctx)
    local i = 0
    while i < 50 do
        ctx.stderr:write(string.format("[tick %d] hello from app\n", i))
        hull.sleep(50)
        i = i + 1
    end
    return 0
end)
APP

    OUT=$("${DRIVE}" "hull dev" "%s400%q" -- \
          "${HULL_BIN}" dev --tui "${DEV_TMP}/app.lua" 2>/dev/null)
    case "${OUT}" in
        *"hull dev"*"app="*)
            pass "hull dev --tui renders title + status"
            ;;
        *)
            fail "hull dev --tui missing title/status"
            ;;
    esac

    # Wait long enough for several child ticks to appear in the log.
    OUT=$("${DRIVE}" "[tick" "%s500%q" -- \
          "${HULL_BIN}" dev --tui "${DEV_TMP}/app.lua" 2>/dev/null)
    case "${OUT}" in
        *"[tick"*"hello from app"*)
            pass "hull dev --tui streams child stderr into the log"
            ;;
        *)
            fail "hull dev --tui did not stream child output (got: ${OUT})"
            ;;
    esac

    # Manual reload via 'r' key. The reload marker should appear.
    OUT=$("${DRIVE}" "── reload" "%s300r%s400%q" -- \
          "${HULL_BIN}" dev --tui "${DEV_TMP}/app.lua" 2>/dev/null)
    case "${OUT}" in
        *"── reload"*)
            pass "hull dev --tui 'r' triggers a reload (marker visible)"
            ;;
        *)
            fail "hull dev --tui 'r' did not produce a reload marker"
            ;;
    esac

    if "${HULL_BIN}" dev --tui "${DEV_TMP}/app.lua" < /dev/null > /dev/null 2>&1; then
        fail "hull dev --tui without a tty should exit non-zero"
    else
        pass "hull dev --tui without a tty exits with a helpful error"
    fi
fi

echo
echo "${PASS}/$((PASS + FAIL)) TUI e2e tests passed"
[ "${FAIL}" -eq 0 ]
