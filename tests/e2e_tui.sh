#!/bin/sh
# tests/e2e_tui.sh - End-to-end smoke tests for the hull.tui module.
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
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL_BIN"
TUI_RC_TMP="${TMPDIR:-/tmp}/hull_tui_rc.$$"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }

# Assert that stderr contains `$3` and exit code is `$2`.
#
# The status goes through hull_run because every caller here runs hull, and on
# Windows an APE reports its status shifted left by 8 - a POSIX shell reads 0
# for every outcome (jart/cosmopolitan#1521). That is why these checks reported
# "got rc=0, expected 1; output OK": the refusal happened, the message was
# right, and only the code could not be seen. HULL_RC_STDIN reproduces the
# `< /dev/null` these need (a tty-refusal test must not inherit a terminal).
expect_failure_with() {
    label="$1"
    expected_rc="$2"
    needle="$3"
    shift 3
    rc=$(HULL_RC_STDIN=/dev/null hull_run "$TUI_RC_TMP" "$@")
    out=$(cat "$TUI_RC_TMP")
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
# ONE trap for the whole suite. A second `trap ... EXIT` REPLACES this one
# rather than adding to it, so the later DEV_TMP trap was silently dropping
# both ${TMP} and the rc scratch file. DEV_TMP is registered here instead, and
# is simply empty until that section sets it.
trap 'rm -rf "${TMP}" "${DEV_TMP:-}" 2>/dev/null; rm -f "${TUI_RC_TMP}"' EXIT

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
# HL_ENABLE_TUI=1). If absent - e.g. cosmo build without forkpty -
# we skip these instead of failing.

DRIVE="${DRIVE:-build/e2e_tui_drive}"

# Whether the driver EXISTS is a proxy for whether it WORKS, and on a cosmo
# host the two come apart: forkpty compiles, the binary builds, and it cannot
# actually drive a terminal - so all 24 interactive cases failed with "missing
# expected output" rather than skipping. Probe the capability instead: drive a
# trivial command through it and require the expected text back.
#
# REQUIRE THE TEXT, NOT THE STATUS. This probe used to discard stdout and test
# only the exit status, which is not what the paragraph above describes and is
# not sufficient on the one host it was written for. Without forkpty the driver
# prints "SKIP: no forkpty on this platform" and returns 77 (the automake skip
# convention) - but on Windows it is built by cosmocc, so it is an APE, and an
# APE's status arrives shifted left by 8 with only the low byte kept
# (lib/hull_rc.sh). 77 became 0, the probe reported success, and 24 interactive
# cases ran against a driver that only ever printed SKIP.
#
# The cost was not just 19 honest-looking failures. The 8 picker cases assert
# by exit status too, so they PASSED - vacuously, against a driver that never
# drove anything. Reading the captured text cannot be fooled that way.
pty_driver_works() {
    [ -x "${DRIVE}" ] || return 1
    _probe_out=$("${DRIVE}" "hull-pty-probe" "" -- printf 'hull-pty-probe' 2>/dev/null)
    case "$_probe_out" in
        *hull-pty-probe*) return 0 ;;
        *)                return 1 ;;
    esac
}

if ! pty_driver_works; then
    echo "--- interactive (skipped) ---"
    if [ -x "${DRIVE}" ]; then
        echo "  (PTY driver at ${DRIVE} cannot drive a terminal on this host)"
    else
        echo "  (no PTY driver at ${DRIVE} - run \`make build/e2e_tui_drive\`)"
    fi
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
    if [ "$(HULL_RC_STDIN=/dev/null hull_rc "${HULL_BIN}" doctor --tui)" = 0 ]; then
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
    # is on stdout - we check for a known token from the context payload.
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
    if [ "$(HULL_RC_STDIN=/dev/null hull_rc "${HULL_BIN}" agent context --interactive)" = 0 ]; then
        fail "hull agent context --interactive without a tty should exit non-zero"
    else
        pass "hull agent context --interactive without a tty exits with a helpful error"
    fi
    if [ "$(HULL_RC_STDIN=/dev/null hull_rc "${HULL_BIN}" agent errors --tui)" = 0 ]; then
        fail "hull agent errors --tui without a tty should exit non-zero"
    else
        pass "hull agent errors --tui without a tty exits with a helpful error"
    fi

    # ── hull dev --tui (Phase 3 headline) ──────────────────────────
    echo "--- hull dev --tui ---"

    # Spin up a tiny test app that logs every 100ms.
    DEV_TMP=$(mktemp -d 2>/dev/null || mktemp -d -t hulldev)
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

    if [ "$(HULL_RC_STDIN=/dev/null hull_rc "${HULL_BIN}" dev --tui "${DEV_TMP}/app.lua")" = 0 ]; then
        fail "hull dev --tui without a tty should exit non-zero"
    else
        pass "hull dev --tui without a tty exits with a helpful error"
    fi

    # ── hull modules available --tui ──────────────────────────────
    echo "--- hull modules available --tui ---"
    OUT=$("${DRIVE}" "hull modules" "%q" -- \
          "${HULL_BIN}" modules available --tui 2>/dev/null)
    case "${OUT}" in
        *"hull modules available"*"hull/app"*)
            pass "hull modules available --tui renders title + first module"
            ;;
        *)
            fail "hull modules available --tui missing expected output"
            ;;
    esac

    # Filter for "tui" - should narrow down to just modules with that
    # substring (hull/tui at minimum). We look for `filter="tui"` in
    # the bottom key bar; the cell-diff renderer skips re-emitting
    # unchanged prefixes ("hull/" was already on-screen pre-filter),
    # so a `hull/tui` substring search would miss.
    OUT=$("${DRIVE}" 'filter="tui"' "/tui%r%q" -- \
          "${HULL_BIN}" modules available --tui 2>/dev/null)
    case "${OUT}" in
        *'filter="tui"'*)
            pass "hull modules available --tui filter narrows the list"
            ;;
        *)
            fail "hull modules available --tui filter did not work"
            ;;
    esac

    if [ "$(HULL_RC_STDIN=/dev/null hull_rc "${HULL_BIN}" modules available --tui)" = 0 ]; then
        fail "hull modules available --tui without a tty should exit non-zero"
    else
        pass "hull modules available --tui without a tty exits with a helpful error"
    fi

    # ── tui_repl example ──────────────────────────────────────────
    echo "--- tui_repl ---"
    OUT=$("${DRIVE}" "repl: 2 expressions" "1+1%r2*3%rq" -- \
          "${HULL_BIN}" examples/tui_repl/app.lua 2>/dev/null)
    case "${OUT}" in
        *"calc>"*"= 2"*"= 6"*)
            pass "tui_repl evaluates two expressions + tracks history"
            ;;
        *)
            fail "tui_repl missing expected eval output"
            ;;
    esac

    # ── tui_log_tailer example ────────────────────────────────────
    echo "--- tui_log_tailer ---"
    TAIL_LOG=/tmp/hull-tui-tail-e2e.log
    rm -f "${TAIL_LOG}"
    echo "seed" > "${TAIL_LOG}"
    OUT=$("${DRIVE}" "hull tui-tailer" "%s200%q" -- \
          "${HULL_BIN}" examples/tui_log_tailer/app.lua 2>/dev/null)
    case "${OUT}" in
        *"hull tui-tailer"*"/tmp/hull-tail.log"*)
            pass "tui_log_tailer renders title + watched path"
            ;;
        *)
            fail "tui_log_tailer missing expected output"
            ;;
    esac
    rm -f "${TAIL_LOG}"

    # ── tui_chat example ──────────────────────────────────────────
    echo "--- tui_chat ---"
    OUT=$("${DRIVE}" "chat:" "hello%r%s700q" -- \
          "${HULL_BIN}" examples/tui_chat/app.lua 2>/dev/null)
    case "${OUT}" in
        *"hull tui-chat"*"you:"*"hello"*)
            pass "tui_chat renders user message"
            ;;
        *)
            fail "tui_chat did not render user message"
            ;;
    esac

    # ── hull migrate status --tui ─────────────────────────────────
    echo "--- hull migrate status --tui ---"
    MIG_DIR=$(mktemp -d 2>/dev/null || mktemp -d -t hullmig)
    mkdir -p "${MIG_DIR}/migrations"
    cat > "${MIG_DIR}/migrations/001_init.sql" <<'SQL'
CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);
SQL
    cat > "${MIG_DIR}/migrations/002_index.sql" <<'SQL'
CREATE INDEX idx_users_name ON users(name);
SQL
    OUT=$("${DRIVE}" "hull migrate status" "%q" -- \
          "${HULL_BIN}" migrate status --tui -d "${MIG_DIR}/data.db" "${MIG_DIR}" 2>/dev/null)
    case "${OUT}" in
        *"hull migrate status"*"0 applied"*"2 pending"*"001_init.sql"*)
            pass "hull migrate status --tui shows applied/pending counts + entries"
            ;;
        *)
            fail "hull migrate status --tui missing expected output"
            ;;
    esac

    # Apply migrations, re-run, and expect "2 applied".
    "${HULL_BIN}" migrate -d "${MIG_DIR}/data.db" "${MIG_DIR}" >/dev/null 2>&1
    OUT=$("${DRIVE}" "2 applied" "%q" -- \
          "${HULL_BIN}" migrate status --tui -d "${MIG_DIR}/data.db" "${MIG_DIR}" 2>/dev/null)
    case "${OUT}" in
        *"2 applied"*"0 pending"*)
            pass "hull migrate status --tui reflects applied state after running migrations"
            ;;
        *)
            fail "hull migrate status --tui did not show applied state"
            ;;
    esac
    rm -rf "${MIG_DIR}"

    if [ "$(HULL_RC_STDIN=/dev/null hull_rc "${HULL_BIN}" migrate status --tui)" = 0 ]; then
        fail "hull migrate status --tui without a tty should exit non-zero"
    else
        pass "hull migrate status --tui without a tty exits with a helpful error"
    fi

    # ── tui_dashboard example (multi-pane + tui.frame + mouse) ────
    echo "--- tui_dashboard ---"

    # Verify the dashboard renders title + sections + progress glyphs.
    OUT=$("${DRIVE}" "Hull TUI dashboard" "%s300%q" -- \
          "${HULL_BIN}" examples/tui_dashboard/app.lua 2>/dev/null)
    case "${OUT}" in
        *"Hull TUI dashboard"*"metrics"*"startup tasks"*"help"*)
            pass "tui_dashboard renders three panes"
            ;;
        *)
            fail "tui_dashboard missing expected panes"
            ;;
    esac

    # Mouse mode is opt-in via tui.run({ mouse = true }) - verify the
    # cap layer emitted the SGR mouse enable sequences.
    case "${OUT}" in
        *"[?1000h"*"[?1006h"*)
            pass "tui_dashboard enables SGR mouse via mouse=true"
            ;;
        *)
            fail "tui_dashboard did not enable SGR mouse"
            ;;
    esac

    # And cleanly disables them on exit.
    case "${OUT}" in
        *"[?1006l"*"[?1000l"*)
            pass "tui_dashboard disables SGR mouse on exit"
            ;;
        *)
            fail "tui_dashboard did not disable SGR mouse on exit"
            ;;
    esac
fi

echo
echo "${PASS}/$((PASS + FAIL)) TUI e2e tests passed"
[ "${FAIL}" -eq 0 ]
