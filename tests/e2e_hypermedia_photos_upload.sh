#!/bin/sh
# E2E tests for the photo upload feature added to
# examples/hypermedia_photos (§1.5.b-5). Drives a real `hull dev`
# server via curl through the full HTML/HTMX flow:
#
#   1. GET / → bootstrap session + CSRF token (parsed from the form).
#   2. POST /entries → create a entry.
#   3. POST /entries/:id/photos with multipart/form-data → upload a PNG.
#   4. GET /entries/:id/photos/:att_id → retrieve, verify bytes.
#   5. GET / → photo appears in the listing.
#   6. DELETE /entries/:id/photos/:att_id → remove.
#   7. GET / → photo is gone.
#
# Both Lua and JS variants. The example apps share templates, so the
# only delta is which entry point we point hull at.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
APP_DIR="$SRCDIR/examples/hypermedia_photos"
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}
SERVER_PID=""

if [ ! -x "$HULL" ]; then
    echo "e2e_hypermedia_photos_upload: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ — $2}"; }

contains() {
    case "$3" in
        *"$2"*) pass "$1" ;;
        *)      fail "$1" "expected substring '$2' in: $3" ;;
    esac
}

wait_for_server() {
    _port=$1
    for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        # The app's GET / responds 200 once it's serving.
        if curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$_port/" 2>/dev/null \
            | grep -q '^[23]'; then
            return 0
        fi
        sleep 0.3
    done
    echo "  server did not start on port $_port"
    return 1
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

TMPDIR_WORK=$(mktemp -d 2>/dev/null || mktemp -d -t hull_hmd_upload)
cleanup() {
    stop_server
    if [ -n "$TMPDIR_WORK" ] && [ -d "$TMPDIR_WORK" ]; then
        rm -rf "$TMPDIR_WORK"
    fi
}
trap cleanup EXIT

# Real PNG header — same shape as the e2e_attachment fixture.
printf '\211PNG\r\n\032\n\x00\x00\x00\x0DIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00' > "$TMPDIR_WORK/photo.png"
PHOTO_SIZE=$(wc -c < "$TMPDIR_WORK/photo.png" | tr -d ' ')

# We run hull from a private working dir so each suite gets a fresh
# DB + blob store (the example expects ./data and ./db.sqlite to be
# under the cwd). Copy the example tree there, into a per-runtime
# subdir, so they don't trample each other's state.
prepare_workdir() {
    SUITE_DIR=$1
    KEEP_FILE=$2
    rm -rf "$TMPDIR_WORK/$SUITE_DIR"
    mkdir -p "$TMPDIR_WORK/$SUITE_DIR"
    cp -r "$APP_DIR"/* "$TMPDIR_WORK/$SUITE_DIR/"
    # Remove the OTHER runtime's entry so hull picks the one we want.
    case "$KEEP_FILE" in
        app.lua) rm -f "$TMPDIR_WORK/$SUITE_DIR/app.js" ;;
        app.js)  rm -f "$TMPDIR_WORK/$SUITE_DIR/app.lua" ;;
    esac
    # Pre-create data/ so Linux Landlock unveil can pin it.
    mkdir -p "$TMPDIR_WORK/$SUITE_DIR/data"
}

run_suite() {
    SUITE=$1
    ENTRY=$2
    PORT=$3
    echo
    echo "=== E2E: hypermedia_photos upload ($SUITE) ==="

    prepare_workdir "$SUITE" "$ENTRY"
    cd "$TMPDIR_WORK/$SUITE"
    # Direct hull invocation (NOT `hull dev`) — same pattern as
    # tests/e2e_examples.sh. The dev watcher mode adds steps we
    # don't need + has its own brittleness; plain `hull <entry>`
    # picks the entry point, applies sandbox, and serves.
    "$HULL" -p "$PORT" "$ENTRY" > "$TMPDIR_WORK/server-$SUITE.log" 2>&1 &
    SERVER_PID=$!
    cd - >/dev/null
    if ! wait_for_server "$PORT"; then
        fail "$SUITE — server startup"
        head -40 "$TMPDIR_WORK/server-$SUITE.log"
        stop_server
        return
    fi

    COOKIE_JAR="$TMPDIR_WORK/cookies-$SUITE.txt"
    rm -f "$COOKIE_JAR"

    # 1. GET / — bootstrap session cookie + CSRF token. The CSRF
    # token is rendered into the new-entry form as
    #   <input type="hidden" name="_csrf" value="...">
    HOME=$(curl -s -c "$COOKIE_JAR" "http://127.0.0.1:$PORT/")
    CSRF=$(printf '%s' "$HOME" | sed -n 's/.*name="_csrf" value="\([^"]*\)".*/\1/p' | head -1)
    if [ -z "$CSRF" ]; then
        fail "$SUITE — couldn't extract CSRF token from /"
        stop_server; return
    fi
    pass "$SUITE bootstrap: cookie + CSRF token acquired"

    # 2. POST /entries — create a entry. HX-Request: true tells the app
    # to return a fragment (the new row) instead of redirecting.
    R_TODO=$(curl -s -b "$COOKIE_JAR" -c "$COOKIE_JAR" \
        -X POST "http://127.0.0.1:$PORT/entries" \
        -H "HX-Request: true" \
        -H "X-CSRF-Token: $CSRF" \
        -d "title=Photo+test&_csrf=$CSRF")
    contains "$SUITE create entry" "Photo test" "$R_TODO"
    TODO_ID=$(printf '%s' "$R_TODO" | sed -n 's/.*id="entry-\([0-9]*\)".*/\1/p' | head -1)
    if [ -z "$TODO_ID" ]; then
        fail "$SUITE — couldn't extract entry id from POST response"
        stop_server; return
    fi

    # 3. POST /entries/:id/photos — multipart upload.
    R_UP=$(curl -s -b "$COOKIE_JAR" \
        -X POST "http://127.0.0.1:$PORT/entries/$TODO_ID/photos" \
        -H "HX-Request: true" \
        -H "X-CSRF-Token: $CSRF" \
        -F "photo=@$TMPDIR_WORK/photo.png" \
        -F "_csrf=$CSRF")
    contains "$SUITE upload: strip rendered" 'class="attachment-strip"' "$R_UP"
    contains "$SUITE upload: photo.png appears" "photo.png" "$R_UP"

    # Extract attachment id from rendered strip (`id="att-<hex>"`).
    ATT_ID=$(printf '%s' "$R_UP" | sed -n 's/.*id="att-\([0-9a-f]\{32\}\)".*/\1/p' | head -1)
    if [ -z "$ATT_ID" ]; then
        fail "$SUITE — couldn't extract attachment id"
        echo "    response: $R_UP"
        stop_server; return
    fi

    # 4. GET /entries/:id/photos/:att_id — bytes round-trip.
    curl -s -b "$COOKIE_JAR" \
        -o "$TMPDIR_WORK/back-$SUITE.png" \
        "http://127.0.0.1:$PORT/entries/$TODO_ID/photos/$ATT_ID"
    if cmp -s "$TMPDIR_WORK/photo.png" "$TMPDIR_WORK/back-$SUITE.png"; then
        pass "$SUITE serve: bytes round-trip"
    else
        fail "$SUITE serve: bytes round-trip"
    fi

    # 5. GET / — photo appears in the home listing.
    HOME2=$(curl -s -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/")
    contains "$SUITE listing shows attachment" "att-$ATT_ID" "$HOME2"
    contains "$SUITE listing shows filename"   "photo.png"    "$HOME2"

    # Auth gate: a GUESSED attachment id (not attached to this entry)
    # must hit the default-deny branch and return 4xx, NOT leak bytes.
    # The demo's auth_check gates on join-row ownership, so any
    # arbitrary 32-hex string that isn't actually attached fails.
    GUESS=$(curl -s -o /dev/null -w '%{http_code}' \
        -b "$COOKIE_JAR" \
        "http://127.0.0.1:$PORT/entries/$TODO_ID/photos/00000000000000000000000000000000")
    case "$GUESS" in
        403|404) pass "$SUITE auth gate rejects guessed id (got $GUESS)" ;;
        *)       fail "$SUITE auth gate rejects guessed id" "got $GUESS" ;;
    esac

    # 6. DELETE /entries/:id/photos/:att_id — remove.
    DEL_STATUS=$(curl -s -b "$COOKIE_JAR" -o /dev/null -w '%{http_code}' \
        -X DELETE "http://127.0.0.1:$PORT/entries/$TODO_ID/photos/$ATT_ID" \
        -H "HX-Request: true" \
        -H "X-CSRF-Token: $CSRF")
    contains "$SUITE delete: 200" "200" "$DEL_STATUS"

    # 7. Listing no longer mentions the attachment id.
    HOME3=$(curl -s -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/")
    case "$HOME3" in
        *"att-$ATT_ID"*) fail "$SUITE listing still shows attachment after delete" ;;
        *)               pass "$SUITE listing no longer shows attachment" ;;
    esac

    # 8. Serve after delete → 403 (auth_check fails because the join
    # row was removed). attachment metadata might also be gone if no
    # other refs existed; both produce 403 or 404 from serve().
    AFTER_STATUS=$(curl -s -b "$COOKIE_JAR" -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:$PORT/entries/$TODO_ID/photos/$ATT_ID")
    case "$AFTER_STATUS" in
        403|404) pass "$SUITE serve after delete: 4xx (got $AFTER_STATUS)" ;;
        *)       fail "$SUITE serve after delete" "got $AFTER_STATUS" ;;
    esac

    stop_server
}

if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "lua" ]; then
    run_suite lua app.lua 19890
fi
if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "js" ]; then
    run_suite js app.js 19891
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
