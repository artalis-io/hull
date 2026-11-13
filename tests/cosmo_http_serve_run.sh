#!/bin/sh
# cosmo_http_serve_run.sh - ACCEPTANCE TEST second-host leg (0.13.1 PR#1).
# Runs the Windows-produced cosmo APEs ($APES_DIR/{lua,js,main}_windows.com) on
# THIS host (a second, independent APE host - macOS) and ASSERTS: the Lua and JS
# apps serve 'pong'; the app.main control exits without a listener. Proves the
# fixed produced binary is portable, not Windows-run-specific. Exits non-zero on
# any assertion failure. download-artifact strips the exec bit, so chmod first.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
APES="${APES_DIR:-apes}"
fail=0

serves() {  # <bin> <port> -> echoes the /ping body
    bin="$1"; port="$2"
    [ -f "$bin" ] || { echo ""; return; }
    chmod +x "$bin" 2>/dev/null || true
    "$bin" -p "$port" >/dev/null 2>&1 &
    pid=$!
    resp=""
    i=0; while [ $i -lt 20 ]; do
        sleep 0.4
        resp=$(curl -fsS "http://127.0.0.1:$port/ping" 2>/dev/null) && break
        kill -0 "$pid" 2>/dev/null || break
        i=$((i + 1))
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    echo "$resp"
}

# Ports below 32768 (Linux client source ports are 32768-60999, so a port
# there can be held by any client socket); the pid offset keeps runs apart.
PORT_BASE=$((31000 + ($$ % 100) * 3))

echo "== second-host run: $(uname -s -m) =="
if [ "$(serves "$APES/lua_windows.com" "$((PORT_BASE + 0))")" = "pong" ]; then
    echo "PASS lua serves pong"; else echo "FAIL lua did not serve"; fail=1; fi
if [ "$(serves "$APES/js_windows.com" "$((PORT_BASE + 1))")" = "pong" ]; then
    echo "PASS js serves pong"; else echo "FAIL js did not serve"; fail=1; fi
if [ "$(serves "$APES/main_windows.com" "$((PORT_BASE + 2))")" = "pong" ]; then
    echo "FAIL app.main control unexpectedly served"; fail=1
else echo "PASS app.main control did not serve"; fi

[ $fail -eq 0 ] || { echo "cosmo_http_serve_run: acceptance FAILED"; exit 1; }
echo "cosmo_http_serve_run: ALL PASS"
