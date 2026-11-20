#!/bin/sh
# e2e_dev_types.sh - `hull dev`'s type check after each reload, with a
# stand-in compiler (the app's node_modules/.bin/tsc, a shell script), so it
# needs no TypeScript install and depends on nothing a compiler prints but
# the diagnostic line:
#   - on start and after each reload the compiler runs once, in the
#     background, as `--noEmit --pretty false -p <app>`, without the
#     dev-session key in its environment;
#   - its diagnostics are summarised on stderr and served by `hull agent
#     errors` (a line of another shape as text); a fix reports no errors;
#   - an edit stops a check still running;
#   - a tampered .hull/types.json is not served, and stopping hull dev
#     removes it;
#   - --no-types runs nothing; no compiler or no tsconfig.json only says so.
set -eu

HULL=${HULL:-build/hull}
case "$HULL" in /*) ;; *) HULL="$PWD/$HULL" ;; esac
WORK=$(mktemp -d)
HOME="$WORK/home"
mkdir -p "$HOME"
export HOME
fail=0
DEVPID=
cleanup() { [ -n "$DEVPID" ] && kill "$DEVPID" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }
has() { grep -qF -- "$2" "$1"; }
# wait_for FILE TEXT [SECONDS]: until FILE holds TEXT.
wait_for() {
    n=0; lim=$(( ${3:-20} * 10 ))
    until grep -qF -- "$2" "$1" 2>/dev/null || [ $n -ge $lim ]; do sleep 0.1; n=$((n+1)); done
    grep -qF -- "$2" "$1" 2>/dev/null
}
# count FILE TEXT: lines of FILE holding TEXT.
count() { grep -cF -- "$2" "$1" 2>/dev/null || true; }
# wait_more FILE TEXT N [SECONDS]: until FILE holds TEXT on more than N lines.
wait_more() {
    n=0; lim=$(( ${4:-20} * 10 ))
    until [ "$(count "$1" "$2")" -gt "$3" ] || [ $n -ge $lim ]; do sleep 0.1; n=$((n+1)); done
    [ "$(count "$1" "$2")" -gt "$3" ]
}

app="$WORK/app"
mkdir -p "$app/node_modules/.bin"
cat > "$app/app.ts" <<'TS'
import { app } from "hull:app";
app.manifest({ modules: ["hull/http-server@1"] });
app.get("/", (_req, res) => res.text("ok"));
TS
printf '{ "compilerOptions": { "noEmit": true } }\n' > "$app/tsconfig.json"
CALLS="$WORK/calls.log"
ENVS="$WORK/env.log"
cat > "$app/node_modules/.bin/tsc" <<SH
#!/bin/sh
echo "\$*" >> "$CALLS"
echo "\$(date +%s) pid \$\$ ppid \$PPID dev \$(ps -o ppid= -p \$PPID 2>/dev/null | tr -d ' ')" >> "$WORK/who.log"
env | grep -c HULL_DEV_SESSION >> "$ENVS"
dir=\$5
if grep -q SLOWTYPE "\$dir/app.ts"; then echo \$\$ > "$WORK/slow.pid"; sleep 30; fi
if grep -q C1TYPE "\$dir/app.ts"; then
    printf 'app.ts(1,2): error TS2345: quoted \302\233x and \177y\\n'
    exit 2
fi
if grep -q BADTYPE "\$dir/app.ts"; then
    echo "app.ts(3,7): error TS2322: Type 'string' is not assignable to type 'number'."
    echo "a line of another shape"
    exit 2
fi
exit 0
SH
chmod +x "$app/node_modules/.bin/tsc"

port=$((20000 + $$ % 20000))
start_dev() {   # $1: extra flags; log in $WORK/dev.log
    : > "$WORK/dev.log"
    (cd "$app" && exec "$HULL" dev --agent $1 -p "$port") > "$WORK/dev.log" 2>&1 &
    DEVPID=$!
    DEVS="${DEVS:-} $DEVPID"
}
stop_dev() { kill "$DEVPID" 2>/dev/null; wait "$DEVPID" 2>/dev/null || true; DEVPID=; port=$((port + 1)); }
errors() { (cd "$app" && "$HULL" agent errors .) > "$WORK/errors.json" 2>&1 || true; }

echo "== hull dev: the app's own compiler needs --types (audit 49)"
# The app's node_modules/.bin compiler is the repository's code: a plain
# hull dev never runs it (it ran unsandboxed, by default). PATH holds no
# compiler here (a CI host may have one, which hull dev would rightly use).
: > "$CALLS"
: > "$WORK/dev.log"
(cd "$app" && PATH=/usr/bin:/bin exec "$HULL" dev -p "$port") > "$WORK/dev.log" 2>&1 &
DEVPID=$!
wait_for "$WORK/dev.log" "listening on" || true
sleep 2
stop_dev
[ ! -s "$CALLS" ] && has "$WORK/dev.log" "is run only with --types" &&
    ok "dev_audit49.app_local_checker_not_run" ||
    { bad "dev_audit49.app_local_checker_not_run"; cat "$WORK/dev.log" "$CALLS"; }
# Nor one PATH leads to inside the app tree.
: > "$CALLS"
: > "$WORK/dev.log"
(cd "$app" && PATH="$app/node_modules/.bin:/usr/bin:/bin" exec "$HULL" dev -p "$port") > "$WORK/dev.log" 2>&1 &
DEVPID=$!
wait_for "$WORK/dev.log" "listening on" || true
sleep 2
stop_dev
[ ! -s "$CALLS" ] && ok "dev_audit49.path_into_app_not_run" ||
    { bad "dev_audit49.path_into_app_not_run"; cat "$WORK/dev.log" "$CALLS"; }

echo "== hull dev --types: the type check"
start_dev "--types"
if wait_for "$WORK/dev.log" "[hull:dev] types: no errors"; then
    has "$WORK/dev.log" "type-checking with ./node_modules/.bin/tsc after each reload" &&
        [ "$(head -n 1 "$CALLS")" = "--noEmit --pretty false -p ." ] &&
        ok "on start: one run, --noEmit --pretty false -p <app>, no errors" ||
        { bad "start"; cat "$WORK/dev.log" "$CALLS"; }
else
    bad "no type check on start"; cat "$WORK/dev.log"
fi
errors
has "$WORK/errors.json" '"types":{"status":"ok","count":0,"errors":0' &&
    ok "hull agent errors: status ok" || { bad "agent errors ok"; cat "$WORK/errors.json"; }

printf '// BADTYPE\n' >> "$app/app.ts"
if wait_for "$WORK/dev.log" "[hull:dev] types: 1 error"; then
    has "$WORK/dev.log" "[hull:dev]   app.ts:3:7 TS2322 Type 'string' is not assignable" &&
        ok "after an edit: the error is summarised" || { bad "summary"; cat "$WORK/dev.log"; }
else
    bad "no report after an edit"; cat "$WORK/dev.log"
fi
errors
has "$WORK/errors.json" '"status":"errors","count":2,"errors":1' &&
    has "$WORK/errors.json" '{"file":"app.ts","line":3,"col":7,"severity":"error","code":"TS2322"' &&
    has "$WORK/errors.json" '{"message":"a line of another shape"}' &&
    ok "hull agent errors: the diagnostic, and the other line as text" || { bad "agent errors"; cat "$WORK/errors.json"; }

# A diagnostic quoting DEL / C1 text: written escaped, so the genuine file
# is still served (audit 49: it was reported as not Hull's).
sed 's/BADTYPE/C1TYPE/' "$app/app.ts" > "$WORK/app.ts" && cp "$WORK/app.ts" "$app/app.ts"
if wait_for "$WORK/dev.log" "TS2345"; then
    sleep 0.5
    errors
    has "$WORK/errors.json" '"code":"TS2345"' && ! has "$WORK/errors.json" "types_ignored" &&
        ok "dev_audit49.types_c1_del_served" || { bad "dev_audit49.types_c1_del_served"; cat "$WORK/errors.json"; }
else
    bad "dev_audit49.types_c1_del_served (no report)"; cat "$WORK/dev.log"
fi
sed 's/C1TYPE/BADTYPE/' "$app/app.ts" > "$WORK/app.ts" && cp "$WORK/app.ts" "$app/app.ts"
n=0
until errors && has "$WORK/errors.json" '"code":"TS2322"' || [ $n -ge 100 ]; do sleep 0.2; n=$((n+1)); done

cp "$app/.hull/types.json" "$WORK/types.saved"
sed 's/TS2322/TS0000/' "$WORK/types.saved" > "$app/.hull/types.json"
errors
has "$WORK/errors.json" '"types_ignored":".hull/types.json was ignored' && ! has "$WORK/errors.json" "TS0000" &&
    ok "a tampered types.json is not served" || { bad "tamper"; cat "$WORK/errors.json"; }

sed 's/BADTYPE/SLOWTYPE/' "$app/app.ts" > "$WORK/app.ts" && cp "$WORK/app.ts" "$app/app.ts"
if wait_for "$WORK/slow.pid" ""; then
    n=0; until [ -s "$WORK/slow.pid" ] || [ $n -ge 100 ]; do sleep 0.1; n=$((n+1)); done
    slow=$(cat "$WORK/slow.pid")
    clean=$(count "$WORK/dev.log" "[hull:dev] types: no errors")
    sed '/SLOWTYPE/d' "$app/app.ts" > "$WORK/app.ts" && cp "$WORK/app.ts" "$app/app.ts"
    n=0; while kill -0 "$slow" 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done
    ! kill -0 "$slow" 2>/dev/null && wait_more "$WORK/dev.log" "[hull:dev] types: no errors" "$clean" &&
        ok "an edit stops a check still running" || bad "the slow check outlived the edit"
else
    bad "the slow check did not start"
fi
[ "$(grep -cv '^0$' "$ENVS")" -eq 0 ] && ok "the compiler never sees the dev-session key" ||
    { bad "HULL_DEV_SESSION reached the compiler"; cat "$ENVS"; }
stop_dev
[ ! -e "$app/.hull/types.json" ] && ok "stopping hull dev removes types.json" || bad "types.json left behind"

echo "== hull dev: off"
: > "$CALLS"
start_dev "--types --no-types"
wait_for "$WORK/dev.log" "listening on" || true
sleep 2
stop_dev
[ ! -s "$CALLS" ] && ! has "$WORK/dev.log" "type-checking with" && ok "--no-types runs nothing" ||
    { bad "--no-types"; cat "$WORK/dev.log" "$CALLS" "$WORK/who.log"; echo "devs: $DEVS"; }

mv "$app/node_modules" "$WORK/node_modules.off"
: > "$WORK/dev.log"
(cd "$app" && PATH=/usr/bin:/bin exec "$HULL" dev -p "$port") > "$WORK/dev.log" 2>&1 &
DEVPID=$!
wait_for "$WORK/dev.log" "listening on" || true
stop_dev
has "$WORK/dev.log" "tsconfig.json but no TypeScript compiler (tsgo or tsc) installed" &&
    ok "no compiler: one line" || { bad "no compiler"; cat "$WORK/dev.log"; }

mv "$WORK/node_modules.off" "$app/node_modules"
mv "$app/tsconfig.json" "$WORK/tsconfig.off"
: > "$CALLS"
start_dev "--types"
wait_for "$WORK/dev.log" "listening on" || true
sleep 2
stop_dev
[ ! -s "$CALLS" ] && ! has "$WORK/dev.log" "types" && ok "no tsconfig.json: nothing" ||
    { bad "no tsconfig"; cat "$WORK/dev.log"; }

[ "$fail" -eq 0 ] && echo "e2e_dev_types: OK" || { echo "e2e_dev_types: FAILED"; exit 1; }
