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
# MODE=dev (default): launches `hull <app.lua>` so static / templates
# / migrations come off disk. Fast iteration; what `make dev` runs.
# MODE=build: runs `hull build` on each example first, then launches
# the resulting standalone binary. Exercises the embedded-VFS code
# path - i.e., what end users will actually ship. Catches regressions
# in build/embed/VFS-lookup that dev-mode can't see (e.g., a static
# file that serves from disk in dev but didn't make it into the
# embedded entries array).
MODE="${MODE:-dev}"
BUILD_OUT_DIR="${TMPDIR:-/tmp}/hull-htmx-pw-bin"
# Failure artifacts (traces + final-page screenshots). Living under
# build/ keeps them out of git (build/ is gitignored) and at a
# predictable path so CI can upload via actions/upload-artifact
# without env juggling. Cleared at the start of every run so we
# never confuse a stale failure with a fresh one. Override the
# default with HULL_PW_ARTIFACTS=/some/path if needed.
ARTIFACT_DIR="${HULL_PW_ARTIFACTS:-$SRCDIR/build/playwright-artifacts}"
export HULL_PW_ARTIFACTS="$ARTIFACT_DIR"
PORT_WIDGETS=${PORT_WIDGETS:-19960}
PORT_PHOTOS_LUA=${PORT_PHOTOS_LUA:-19961}
PORT_PHOTOS_JS=${PORT_PHOTOS_JS:-19962}
DB_WIDGETS="${TMPDIR:-/tmp}/hull-htmx-pw-widgets.db"
DB_PHOTOS_LUA="${TMPDIR:-/tmp}/hull-htmx-pw-photos-lua.db"
DB_PHOTOS_JS="${TMPDIR:-/tmp}/hull-htmx-pw-photos-js.db"
PID_WIDGETS=""
PID_PHOTOS_LUA=""
PID_PHOTOS_JS=""

mkdir -p "$LOG_DIR"
rm -rf "$ARTIFACT_DIR" && mkdir -p "$ARTIFACT_DIR"

cleanup() {
    [ -n "$PID_WIDGETS"    ] && kill "$PID_WIDGETS"    2>/dev/null || true
    [ -n "$PID_PHOTOS_LUA" ] && kill "$PID_PHOTOS_LUA" 2>/dev/null || true
    [ -n "$PID_PHOTOS_JS"  ] && kill "$PID_PHOTOS_JS"  2>/dev/null || true
    rm -f "$DB_WIDGETS"    "$DB_WIDGETS-journal" \
          "$DB_PHOTOS_LUA" "$DB_PHOTOS_LUA-journal" \
          "$DB_PHOTOS_JS"  "$DB_PHOTOS_JS-journal"
    # Built binaries from MODE=build land under a temp dir; sweep
    # them so a follow-up dev-mode run isn't confused by stale
    # binaries left in the examples tree.
    rm -rf "$BUILD_OUT_DIR"
}
trap cleanup EXIT INT TERM

# ── Prereqs ──────────────────────────────────────────────────────
if [ ! -x "$HULL" ]; then
    echo "e2e_htmx_playwright: hull binary not found at $HULL - run 'make' first"
    exit 1
fi

if ! command -v node >/dev/null 2>&1; then
    echo "e2e_htmx_playwright: node not found - SKIPPING (browser tests need node >=18)"
    exit 0
fi
NODE_MAJOR=$(node -p "process.versions.node.split('.')[0]" 2>/dev/null || echo 0)
if [ "$NODE_MAJOR" -lt 18 ]; then
    echo "e2e_htmx_playwright: node $NODE_MAJOR < 18 - SKIPPING"
    exit 0
fi

if ! command -v npm >/dev/null 2>&1; then
    echo "e2e_htmx_playwright: npm not found - SKIPPING"
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
  "dependencies": {
    "playwright":           "^1.48.0",
    "@axe-core/playwright": "^4.11.3"
  }
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

# ── Build mode: produce standalone binaries for each example ──
# Each app gets a one-shot `hull build` into BUILD_OUT_DIR. Skips
# entirely in dev mode. Hard-errors when MODE=build but this hull
# was built without EMBED_PLATFORM=1 - there's no graceful path
# (hull build literally can't proceed without the platform lib).
if [ "$MODE" = "build" ]; then
    if ! "$HULL" doctor --json 2>/dev/null | grep -q '"hull_build":"ready"'; then
        echo "e2e_htmx_playwright: MODE=build needs hull built with"
        echo "  make platform && make EMBED_PLATFORM=1"
        echo "(hull doctor says hull_build != \"ready\")"
        exit 1
    fi
    rm -rf "$BUILD_OUT_DIR" && mkdir -p "$BUILD_OUT_DIR"
    echo "── Building example apps for MODE=build ──"
    # --no-verify-platform: this hull's platform lib hasn't been
    # signed (locally built), so verification would fail. Apps
    # built this way must also be RUN with --no-verify-platform -
    # launch_app passes that downstream.
    build_one() {
        # $1=name  $2=source-dir
        _bn="$1"; _bsrc="$2"
        echo "  $_bn -> $BUILD_OUT_DIR/$_bn"
        "$HULL" build "$_bsrc" --output "$BUILD_OUT_DIR/$_bn" \
            --no-verify-platform >>"$LOG_DIR/build-$_bn.log" 2>&1 || {
            echo "  FAILED: see $LOG_DIR/build-$_bn.log"
            tail -20 "$LOG_DIR/build-$_bn.log"
            exit 1
        }
    }
    build_one "widgets"    "$SRCDIR/examples/htmx_widgets_register"
    build_one "photos-lua" "$SRCDIR/examples/hypermedia_photos"
    # photos-js: hull build always prefers app.lua when both entry
    # files coexist, so stage a copy without it. cp -a preserves
    # the dir structure (templates/, static/, migrations/) which
    # is what the build step embeds.
    STAGE_JS="$BUILD_OUT_DIR/.stage-photos-js"
    rm -rf "$STAGE_JS"
    cp -a "$SRCDIR/examples/hypermedia_photos" "$STAGE_JS"
    rm -f "$STAGE_JS/app.lua"
    build_one "photos-js" "$STAGE_JS"
    rm -rf "$STAGE_JS"
    echo
fi

# launch_app: dispatch on MODE so the dev-vs-build choice only
# lives in one place. dev runs `hull <app>`, build runs the
# standalone binary with the same -p / -d flags. Directly
# backgrounds the hull/app process (no subshell wrapper) so $!
# captures the real pid, not a wrapper's.
launch_app() {
    # $1=name (widgets|photos-lua|photos-js)  $2=port  $3=db  $4=appfile (dev only)  $5=logfile
    _name="$1"; _port="$2"; _db="$3"; _appfile="$4"; _log="$5"
    if [ "$MODE" = "build" ]; then
        # The standalone binary resolves manifest.fs.write paths
        # ("data/") relative to its CWD. Dev mode side-steps this
        # because hull resolves them relative to the app's source
        # dir. For build mode, give each app a private workdir with
        # a writable data/ - keeps the test isolated and avoids
        # touching the source tree.
        _wd="$BUILD_OUT_DIR/$_name-work"
        rm -rf "$_wd" && mkdir -p "$_wd/data"
        ( cd "$_wd" && exec "$BUILD_OUT_DIR/$_name" -p "$_port" -d "$_db" \
            --no-verify-platform ) >"$_log" 2>&1 &
    else
        "$HULL" -p "$_port" -d "$_db" "$_appfile" >"$_log" 2>&1 &
    fi
    echo $!
}

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
echo "── htmx_widgets_register on :$PORT_WIDGETS (MODE=$MODE) ────────"
rm -f "$DB_WIDGETS" "$DB_WIDGETS-journal"
PID_WIDGETS=$(launch_app "widgets" "$PORT_WIDGETS" "$DB_WIDGETS" \
    "$SRCDIR/examples/htmx_widgets_register/app.lua" "$LOG_DIR/widgets.log")
if ! wait_for "http://127.0.0.1:$PORT_WIDGETS/" "htmx_widgets_register"; then
    echo "--- widgets.log ---"; cat "$LOG_DIR/widgets.log"; exit 1
fi

# ── Spin up hypermedia_photos (Lua) ──────────────────────────────
echo "── hypermedia_photos[lua] on :$PORT_PHOTOS_LUA (MODE=$MODE) ────"
rm -f "$DB_PHOTOS_LUA" "$DB_PHOTOS_LUA-journal"
PID_PHOTOS_LUA=$(launch_app "photos-lua" "$PORT_PHOTOS_LUA" "$DB_PHOTOS_LUA" \
    "$SRCDIR/examples/hypermedia_photos/app.lua" "$LOG_DIR/photos-lua.log")
if ! wait_for "http://127.0.0.1:$PORT_PHOTOS_LUA/" "hypermedia_photos (Lua)"; then
    echo "--- photos-lua.log ---"; cat "$LOG_DIR/photos-lua.log"; exit 1
fi

# ── Spin up hypermedia_photos (JS) ───────────────────────────────
# Same Hull binary, same example dir, just point at app.js instead
# of app.lua so the QuickJS runtime gets selected. Catches Lua/JS
# parity regressions at the browser level - the kind of thing unit
# tests can't see (e.g., a widget JS contract drift between runtimes).
echo "── hypermedia_photos[js] on :$PORT_PHOTOS_JS (MODE=$MODE) ──────"
rm -f "$DB_PHOTOS_JS" "$DB_PHOTOS_JS-journal"
PID_PHOTOS_JS=$(launch_app "photos-js" "$PORT_PHOTOS_JS" "$DB_PHOTOS_JS" \
    "$SRCDIR/examples/hypermedia_photos/app.js" "$LOG_DIR/photos-js.log")
if ! wait_for "http://127.0.0.1:$PORT_PHOTOS_JS/" "hypermedia_photos (JS)"; then
    echo "--- photos-js.log ---"; cat "$LOG_DIR/photos-js.log"; exit 1
fi

# ── Run the playwright suite ─────────────────────────────────────
# ESM ignores NODE_PATH, so the test file dynamic-imports playwright
# from PLAYWRIGHT_DIR/node_modules/playwright. PLAYWRIGHT_BROWSERS_PATH
# is already exported above. The optional 3rd arg to the .mjs is a
# runtime label, used in the output banner and (later) failure
# artifacts so logs distinguish Lua-side from JS-side breakage.
export PLAYWRIGHT_DIR="$PW_DIR"
RC=0
node "$SRCDIR/tests/e2e_htmx_playwright.mjs" widgets \
    "http://127.0.0.1:$PORT_WIDGETS" lua || RC=$?
node "$SRCDIR/tests/e2e_htmx_playwright.mjs" photos \
    "http://127.0.0.1:$PORT_PHOTOS_LUA" lua || RC=$?
node "$SRCDIR/tests/e2e_htmx_playwright.mjs" photos \
    "http://127.0.0.1:$PORT_PHOTOS_JS" js || RC=$?

if [ $RC -ne 0 ]; then
    echo
    echo "── server logs ──"
    echo "--- widgets.log ---";    tail -40 "$LOG_DIR/widgets.log"
    echo "--- photos-lua.log ---"; tail -40 "$LOG_DIR/photos-lua.log"
    echo "--- photos-js.log ---";  tail -40 "$LOG_DIR/photos-js.log"
    echo
    echo "── playwright artifacts ──"
    echo "Saved under: $ARTIFACT_DIR"
    # Inventory so the CI log shows what was captured even if the
    # uploaded artifact bundle is later lost. find -type f keeps
    # output one-line-per-file for easy grep.
    find "$ARTIFACT_DIR" -type f 2>/dev/null | sort | while read -r f; do
        sz=$(wc -c < "$f" | tr -d ' ')
        echo "  ${f#$ARTIFACT_DIR/} (${sz} bytes)"
    done
    echo
    echo "Open trace.zip via: npx playwright show-trace <path-to-trace.zip>"
fi

exit $RC
