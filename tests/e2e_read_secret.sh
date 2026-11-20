#!/bin/sh
# e2e_read_secret.sh: app.main reads a secret from a terminal without echo
# (ctx.stdin:read_secret / ctx.stdin.readSecret, platform gap 4).
#
# For each runtime, sandboxed (the default):
#   - on a terminal (a PTY, through e2e_tui_drive) the prompt is shown, the
#     typed key is NOT echoed, and the line is returned;
#   - echo is back on afterwards: a later ctx.stdin read echoes what is typed;
#   - a Ctrl-C while it waits raises / throws "interrupted" and echo is back
#     on (the app has an interrupt handler, so it goes on);
#   - from a pipe, one line is returned and no prompt is written;
#   - at end of input it returns nil / null;
#   - test.run_main's / test.runMain's stdin gives it one line.
# Before the fix there was no such method: the key showed as it was typed.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
DRIVE="${DRIVE:-build/e2e_tui_drive}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

HULL=$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")
HAVE_DRIVE=1
if [ -x "$DRIVE" ]; then
    DRIVE=$(cd "$(dirname "$DRIVE")" && pwd)/$(basename "$DRIVE")
else
    HAVE_DRIVE=0
    echo "  (no $DRIVE: the terminal cases are skipped)"
fi
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

mkdir -p "$TMPDIR/lua/tests" "$TMPDIR/js/tests"
cat > "$TMPDIR/lua/app.lua" <<'EOF'
local app = require("hull.app")
app.manifest({})
app.main(function(ctx)
    ctx.on_interrupt(function() end)
    local ok, s = pcall(function() return ctx.stdin:read_secret("Key: ") end)
    if not ok then
        ctx.stdout:write("err[" .. (tostring(s):match("read_secret: (%a+)") or "?") .. "]\n")
    else
        ctx.stdout:write("got[" .. tostring(s) .. "]\n")
    end
    if ctx.args[1] == "again" then
        ctx.stdout:write("then[" .. tostring(ctx.stdin:read()) .. "]\n")
    end
    return 0
end)
EOF
cat > "$TMPDIR/lua/tests/test_secret.lua" <<'EOF'
test("read_secret reads a line of opts.stdin", function()
    local r = test.run_main({}, { stdin = "k3y\nmore\n" })
    test.eq(r.code, 0)
    test.eq(r.stdout, "got[k3y]\n")
end)
EOF
cat > "$TMPDIR/js/app.js" <<'EOF'
import { app } from "hull:app";
app.manifest({});
app.main((ctx) => {
    ctx.signal.onabort = () => {};
    try {
        const s = ctx.stdin.readSecret("Key: ");
        ctx.stdout.write("got[" + String(s) + "]\n");
    } catch (e) {
        const m = /readSecret: (\w+)/.exec(String(e.message));
        ctx.stdout.write("err[" + (m ? m[1] : "?") + "]\n");
    }
    if (ctx.args[0] === "again")
        ctx.stdout.write("then[" + String(ctx.stdin.read()) + "]\n");
    return 0;
});
EOF
cat > "$TMPDIR/js/tests/test_secret.js" <<'EOF'
test("readSecret reads a line of opts.stdin", () => {
    const r = test.runMain([], { stdin: "k3y\nmore\n" });
    test.eq(r.code, 0);
    test.eq(r.stdout, "got[k3y]\n");
});
EOF

run_runtime() {
    rt=$1; entry=$2
    echo "-- $rt --"
    dir="$TMPDIR/$rt"

    out=$(cd "$dir" && printf 'pip3d\nnext\n' | "$HULL" "$entry" 2>"$TMPDIR/err")
    if [ "$out" = "got[pip3d]" ] && ! grep -q "Key:" "$TMPDIR/err"; then
        pass "$rt: from a pipe, one line and no prompt"
    else
        fail "$rt: pipe" "$out / $(cat "$TMPDIR/err")"
    fi
    out=$(cd "$dir" && "$HULL" "$entry" </dev/null 2>&1)
    case "$out" in
        *"got[nil]"*|*"got[null]"*) pass "$rt: end of input is nil / null" ;;
        *) fail "$rt: end of input" "$out" ;;
    esac
    out=$(cd "$dir" && "$HULL" test 2>&1) && \
        pass "$rt: test.run_main's stdin gives read_secret a line" || \
        fail "$rt: hull test" "$out"

    [ "$HAVE_DRIVE" = 1 ] || return 0
    if (cd "$dir" && "$DRIVE" "then[visible]" "s3kr1t%r%s300visible%r" -- \
            "$HULL" "$entry" -- again > "$TMPDIR/pty" 2>&1); then
        if grep -q "Key: " "$TMPDIR/pty" && grep -q "got\[s3kr1t\]" "$TMPDIR/pty" && \
           [ "$(grep -c s3kr1t "$TMPDIR/pty")" = 1 ]; then
            pass "$rt: on a terminal, the prompt shows and the key is not echoed"
        else
            fail "$rt: terminal read" "$(cat -v "$TMPDIR/pty")"
        fi
        # "visible" twice: echoed as typed, then printed by the app.
        [ "$(grep -c visible "$TMPDIR/pty")" = 2 ] \
            && pass "$rt: echo is back on for the next read" \
            || fail "$rt: echo after" "$(cat -v "$TMPDIR/pty")"
    else
        fail "$rt: terminal read did not finish" "$(cat -v "$TMPDIR/pty")"
    fi
    if (cd "$dir" && "$DRIVE" "then[visible]" "$(printf '\003')%s300visible%r" -- \
            "$HULL" "$entry" -- again > "$TMPDIR/pty" 2>&1) && \
       grep -q "err\[interrupted\]" "$TMPDIR/pty" && \
       [ "$(grep -c visible "$TMPDIR/pty")" = 2 ]; then
        pass "$rt: Ctrl-C while it waits is 'interrupted', with echo back on"
    else
        fail "$rt: Ctrl-C" "$(cat -v "$TMPDIR/pty")"
    fi
}

run_runtime lua app.lua
run_runtime js app.js

echo ""
echo "e2e_read_secret: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
