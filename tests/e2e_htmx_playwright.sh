#!/bin/sh
# E2E browser tests for the HTMX example apps via Playwright + Chromium.
#
# These catch things curl can't:
#   - CSS actually applies in a real browser (the "Pico var typo
#     in app.css makes the page unstyled" regression).
#   - htmx swaps fire on click / input.
#   - widget JS (toast, confirm, inline-edit, form, …) executes
#     after the swap and respects the strict CSP preset.
#
# Tested apps:
#   - examples/htmx_widgets_register   (every §1.5.g widget)
#   - examples/hypermedia_photos       (real CRUD flow with CSRF /
#                                        sessions / idempotency on top)
#
# Usage:    sh tests/e2e_htmx_playwright.sh
# Requires: build/hull built; node >= 18; npm; outbound network on
#           first run so playwright + chromium download.
#
# First run pulls ~150 MB into tests/.playwright/ (cached for next
# runs; the directory is gitignored). Subsequent runs reuse it.
# Skips with exit 0 if node is missing or too old, so this script
# is safe to plug into CI alongside the existing E2E suite.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PW_DIR="$SRCDIR/tests/.playwright"
LOG_DIR="${TMPDIR:-/tmp}/hull-htmx-pw-logs"
PORT_WIDGETS=${PORT_WIDGETS:-19960}
PORT_PHOTOS=${PORT_PHOTOS:-19961}
DB_WIDGETS="${TMPDIR:-/tmp}/hull-htmx-pw-widgets.db"
DB_PHOTOS="${TMPDIR:-/tmp}/hull-htmx-pw-photos.db"
PID_WIDGETS=""
PID_PHOTOS=""

mkdir -p "$LOG_DIR"

cleanup() {
    [ -n "$PID_WIDGETS" ] && kill "$PID_WIDGETS" 2>/dev/null || true
    [ -n "$PID_PHOTOS"  ] && kill "$PID_PHOTOS"  2>/dev/null || true
    rm -f "$DB_WIDGETS" "$DB_WIDGETS-journal" \
          "$DB_PHOTOS"  "$DB_PHOTOS-journal"
}
trap cleanup EXIT INT TERM

# ── Prereqs ──────────────────────────────────────────────────────
if [ ! -x "$HULL" ]; then
    echo "e2e_htmx_playwright: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

if ! command -v node >/dev/null 2>&1; then
    echo "e2e_htmx_playwright: node not found — SKIPPING (browser tests need node >=18)"
    exit 0
fi
NODE_MAJOR=$(node -p "process.versions.node.split('.')[0]" 2>/dev/null || echo 0)
if [ "$NODE_MAJOR" -lt 18 ]; then
    echo "e2e_htmx_playwright: node $NODE_MAJOR < 18 — SKIPPING"
    exit 0
fi

if ! command -v npm >/dev/null 2>&1; then
    echo "e2e_htmx_playwright: npm not found — SKIPPING"
    exit 0
fi

# ── Install playwright + chromium (cached) ───────────────────────
mkdir -p "$PW_DIR"
# Pin local browser dir so it lives next to the install (don't pollute
# ~/.cache and make cache invalidation harder).
export PLAYWRIGHT_BROWSERS_PATH="$PW_DIR/.browsers"

if [ ! -d "$PW_DIR/node_modules/playwright" ]; then
    echo "e2e_htmx_playwright: first-time install (playwright + chromium, ~150 MB)…"
    cat > "$PW_DIR/package.json" <<'JSON'
{
  "name": "hull-htmx-pw",
  "private": true,
  "type": "module",
  "dependencies": { "playwright": "^1.48.0" }
}
JSON
    ( cd "$PW_DIR" && npm install --no-audit --no-fund --no-progress --silent )
fi

if [ ! -d "$PW_DIR/.browsers" ] || [ -z "$(ls -A "$PW_DIR/.browsers" 2>/dev/null)" ]; then
    echo "e2e_htmx_playwright: downloading chromium…"
    # --with-deps needs sudo on Linux for apt; only attempt it on CI.
    if [ -n "$CI" ] && [ "$(uname -s)" = "Linux" ]; then
        ( cd "$PW_DIR" && npx --yes playwright install --with-deps chromium ) \
            || ( cd "$PW_DIR" && npx --yes playwright install chromium )
    else
        ( cd "$PW_DIR" && npx --yes playwright install chromium )
    fi
fi

# ── wait_for: poll until URL returns 200, give up after ~5 s ─────
wait_for() {
    _url="$1"; _label="$2"
    _i=0
    while [ $_i -lt 25 ]; do
        if curl -sS -o /dev/null -w "%{http_code}" "$_url" 2>/dev/null | grep -q "^200$"; then
            return 0
        fi
        _i=$((_i + 1))
        sleep 0.2
    done
    echo "e2e_htmx_playwright: $_label did not become ready at $_url"
    return 1
}

# ── Spin up htmx_widgets_register ────────────────────────────────
echo "── htmx_widgets_register on :$PORT_WIDGETS ────────────────────"
rm -f "$DB_WIDGETS" "$DB_WIDGETS-journal"
( "$HULL" -p "$PORT_WIDGETS" -d "$DB_WIDGETS" \
    "$SRCDIR/examples/htmx_widgets_register/app.lua" \
    >"$LOG_DIR/widgets.log" 2>&1 ) &
PID_WIDGETS=$!
if ! wait_for "http://127.0.0.1:$PORT_WIDGETS/" "htmx_widgets_register"; then
    echo "--- widgets.log ---"; cat "$LOG_DIR/widgets.log"; exit 1
fi

# ── Spin up hypermedia_photos ────────────────────────────────────
echo "── hypermedia_photos on :$PORT_PHOTOS ──────────────────────────"
rm -f "$DB_PHOTOS" "$DB_PHOTOS-journal"
( "$HULL" -p "$PORT_PHOTOS" -d "$DB_PHOTOS" \
    "$SRCDIR/examples/hypermedia_photos/app.lua" \
    >"$LOG_DIR/photos.log" 2>&1 ) &
PID_PHOTOS=$!
if ! wait_for "http://127.0.0.1:$PORT_PHOTOS/" "hypermedia_photos"; then
    echo "--- photos.log ---"; cat "$LOG_DIR/photos.log"; exit 1
fi

# ── Run the playwright suite ─────────────────────────────────────
# ESM ignores NODE_PATH, so the test file dynamic-imports playwright
# from PLAYWRIGHT_DIR/node_modules/playwright. PLAYWRIGHT_BROWSERS_PATH
# is already exported above.
export PLAYWRIGHT_DIR="$PW_DIR"
RC=0
node "$SRCDIR/tests/e2e_htmx_playwright.mjs" widgets "http://127.0.0.1:$PORT_WIDGETS" \
    || RC=$?
node "$SRCDIR/tests/e2e_htmx_playwright.mjs" photos  "http://127.0.0.1:$PORT_PHOTOS" \
    || RC=$?

if [ $RC -ne 0 ]; then
    echo
    echo "── server logs ──"
    echo "--- widgets.log ---"; tail -40 "$LOG_DIR/widgets.log"
    echo "--- photos.log ---";  tail -40 "$LOG_DIR/photos.log"
fi

exit $RC
