#!/bin/sh
# e2e_check_types.sh - `hull check --types`: the TypeScript compiler the app
# or the user installed, run over the app's tsconfig.json and then
# tests/tsconfig.json with --noEmit, before hull check's other steps.
#   - the lookup order: the app's node_modules/.bin (tsgo, then tsc), then
#     PATH (tsgo, then tsc), with stand-in compilers that record their call;
#   - a compiler's failure fails hull check before its next step, and its
#     status is hull check's;
#   - no tsconfig.json names `hull types`; no compiler says how to get one;
#   - without --types no compiler runs;
#   - with the real compiler (TSC=..., else tests/types/node_modules/.bin/tsc
#     when `make check-types` installed it): examples/cli_modular_ts passes,
#     and a type error in it fails.
set -eu

HULL=${HULL:-build/hull}
case "$HULL" in /*) ;; *) HULL="$PWD/$HULL" ;; esac
ROOT=$PWD
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
HOME="$WORK/home"
mkdir -p "$HOME"
export HOME
fail=0
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }
has() { grep -qF -- "$2" "$1"; }

app="$WORK/app"
cp -R "$ROOT/examples/cli_modular_ts" "$app"
"$HULL" types "$app" > /dev/null 2>&1 || { bad "hull types"; exit 1; }
LOG="$WORK/calls.log"

# fake DIR NAME [STATUS]: a stand-in compiler that logs "NAME ARGS" and exits.
fake() {
    mkdir -p "$1"
    printf '#!/bin/sh\necho "%s $*" >> "%s"\nexit %s\n' "$2" "$LOG" "${3:-0}" > "$1/$2"
    chmod +x "$1/$2"
}
bin="$WORK/bin"
mkdir -p "$bin"
run_check() {   # PATH for the run; output in $WORK/out.txt, status in $rc
    : > "$LOG"
    rc=0; PATH="$1:/usr/bin:/bin" "$HULL" check --types "$app" > "$WORK/out.txt" 2>&1 || rc=$?
}

echo "== hull check --types: which compiler"
fake "$bin" tsc
fake "$bin" tsgo
fake "$app/node_modules/.bin" tsc
fake "$app/node_modules/.bin" tsgo
run_check "$bin"
if [ "$(head -n 1 "$LOG")" = "tsgo --noEmit -p $app" ] &&
   [ "$(sed -n 2p "$LOG")" = "tsgo --noEmit -p $app/tests" ] &&
   has "$WORK/out.txt" "type-checking with $app/node_modules/.bin/tsgo" &&
   has "$WORK/out.txt" "[hull:check] validating manifest..."; then
    ok "the app's tsgo first, over the app then tests/, then the next step"
else
    bad "app tsgo"; cat "$LOG" "$WORK/out.txt"
fi
mv "$app/node_modules/.bin/tsgo" "$WORK/tsgo.off"
run_check "$bin"
[ "$(head -n 1 "$LOG")" = "tsc --noEmit -p $app" ] && ok "then the app's tsc" || { bad "app tsc"; cat "$LOG"; }
mv "$app/node_modules" "$WORK/node_modules.off"
run_check "$bin"
[ "$(head -n 1 "$LOG")" = "tsgo --noEmit -p $app" ] && has "$WORK/out.txt" "type-checking with $bin/tsgo" &&
    ok "then tsgo on PATH" || { bad "PATH tsgo"; cat "$LOG" "$WORK/out.txt"; }
mv "$bin/tsgo" "$WORK/bin_tsgo.off"
run_check "$bin"
[ "$(head -n 1 "$LOG")" = "tsc --noEmit -p $app" ] && ok "then tsc on PATH" || { bad "PATH tsc"; cat "$LOG"; }

echo "== hull check --types: failures"
fake "$bin" tsc 2
run_check "$bin"
[ "$rc" -eq 2 ] && [ "$(wc -l < "$LOG")" -eq 1 ] && has "$WORK/out.txt" "[hull:check] type check failed" &&
    ! has "$WORK/out.txt" "validating manifest" &&
    ok "a failing compiler fails hull check with its status, before the next step" ||
    { bad "failing compiler (rc $rc)"; cat "$LOG" "$WORK/out.txt"; }
run_check "$WORK/empty"
[ "$rc" -ne 0 ] && has "$WORK/out.txt" "no TypeScript compiler found" &&
    has "$WORK/out.txt" "npm install --save-dev typescript" &&
    ok "no compiler: hull check says how to install one" || { bad "no compiler (rc $rc)"; cat "$WORK/out.txt"; }
mkdir -p "$WORK/notypes"
cp "$ROOT/examples/cli_modular_ts/app.ts" "$WORK/notypes/"
fake "$bin" tsc
rc=0; PATH="$bin:/usr/bin:/bin" "$HULL" check --types "$WORK/notypes" > "$WORK/out.txt" 2>&1 || rc=$?
[ "$rc" -ne 0 ] && has "$WORK/out.txt" "no tsconfig.json" && has "$WORK/out.txt" "hull types" &&
    ok "no tsconfig.json: hull check names hull types" || { bad "no tsconfig (rc $rc)"; cat "$WORK/out.txt"; }
: > "$LOG"
PATH="$bin:/usr/bin:/bin" "$HULL" check "$app" > "$WORK/out.txt" 2>&1 || true
[ ! -s "$LOG" ] && ! has "$WORK/out.txt" "checking types" && ok "without --types no compiler runs" ||
    { bad "ran without --types"; cat "$LOG"; }

echo "== hull check --types: the real compiler"
TSC=${TSC:-$ROOT/tests/types/node_modules/.bin/tsc}
if [ ! -x "$TSC" ] && [ "${HULL_REQUIRE_TSC:-0}" = "1" ]; then
    bad "no TypeScript compiler at $TSC (HULL_REQUIRE_TSC=1)"
elif [ ! -x "$TSC" ]; then
    echo "  SKIP: no TypeScript compiler (make check-types installs one)"
else
    real="$WORK/real"
    mkdir -p "$real"
    ln -s "$TSC" "$real/tsc"
    tdir=$(dirname "$TSC")
    rc=0; PATH="$real:$tdir:$PATH" "$HULL" check --types "$app" > "$WORK/out.txt" 2>&1 || rc=$?
    has "$WORK/out.txt" "type-checking with" && has "$WORK/out.txt" "validating manifest" &&
        ok "examples/cli_modular_ts type-checks" || { bad "real compiler"; cat "$WORK/out.txt"; }
    printf 'export const bad: number = "s";\n' > "$app/lib/bad.ts"
    rc=0; PATH="$real:$tdir:$PATH" "$HULL" check --types "$app" > "$WORK/out.txt" 2>&1 || rc=$?
    [ "$rc" -ne 0 ] && has "$WORK/out.txt" "TS2322" && has "$WORK/out.txt" "type check failed" &&
        ok "a type error fails hull check" || { bad "type error (rc $rc)"; cat "$WORK/out.txt"; }
fi

[ "$fail" -eq 0 ] && echo "e2e_check_types: OK" || { echo "e2e_check_types: FAILED"; exit 1; }
