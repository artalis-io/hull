#!/bin/sh
# e2e_timers.sh: an async JS timer keeps firing.
#
# A timer handler whose first await is not a Hull operation (`await null`, an
# already-resolved promise) returned a pending promise with no continuation to
# wake it. The timer stayed marked in flight, so it ran once and never again,
# without a word. It now runs the handler's microtasks and goes on. A handler
# awaiting a Hull operation (hull.sleep) is the control: it always worked.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PORT="${PORT:-19897}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

TMPDIR=$(mktemp -d)
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

cat > "$TMPDIR/app.js" <<'EOF'
import { app } from "hull:app";
app.manifest({ modules: ["hull/http-server@1", "hull/timers@1"] });

let microtask = 0;
let slept = 0;
app.every(100, async () => { await null; microtask++; });
app.every(100, async () => { await hull.sleep(5); slept++; });

app.get("/counts", (req, res) => res.text(`microtask=${microtask} slept=${slept}`));
EOF

"$HULL" --no-sandbox -p "$PORT" "$TMPDIR/app.js" >"$TMPDIR/log" 2>&1 &
SERVER_PID=$!
up=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if curl -s -o /dev/null "http://127.0.0.1:$PORT/counts"; then up=1; break; fi
    sleep 0.5
done
[ "$up" = 1 ] || { echo "FAIL: server did not start"; cat "$TMPDIR/log"; exit 1; }

sleep 2
out=$(curl -s "http://127.0.0.1:$PORT/counts")
echo "=== async timers ==="
n=$(printf '%s' "$out" | sed -n 's/.*microtask=\([0-9]*\).*/\1/p')
[ -n "$n" ] && [ "$n" -ge 3 ] \
    && pass "a timer awaiting only microtasks keeps firing ($n runs)" \
    || fail "a timer awaiting only microtasks keeps firing" "$out"
s=$(printf '%s' "$out" | sed -n 's/.*slept=\([0-9]*\).*/\1/p')
[ -n "$s" ] && [ "$s" -ge 3 ] \
    && pass "a timer awaiting hull.sleep keeps firing ($s runs)" \
    || fail "a timer awaiting hull.sleep keeps firing" "$out"

echo ""
echo "e2e_timers: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
