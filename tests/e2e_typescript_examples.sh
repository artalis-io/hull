#!/bin/sh
# e2e_typescript_examples.sh - the TypeScript examples (examples/rest_api_ts,
# examples/cli_modular_ts) through every command an app author runs, on
# copies under a temporary directory:
#   - `hull types` installs the declarations, and the pipeline then erases
#     only the app's own .ts files, never the .d.ts files under types/;
#   - `hull test` passes, and each app runs from source: the CLI's
#     subcommands (imported on demand by name) and the REST API's routes;
#   - `hull manifest` reads a TypeScript entry's manifest; `hull deploy`
#     takes a TypeScript entry;
#   - `hull build` (with the platform library) embeds the erased modules and
#     the binaries run; --sign, and both `hull inspect` and `hull verify`
#     report the app layer VALID.
set -eu

HULL=${HULL:-build/hull}
case "$HULL" in /*) ;; *) HULL="$PWD/$HULL" ;; esac
ROOT=$PWD
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
HOME="$WORK/home"
mkdir -p "$HOME"
export HOME
unset HULL_CACHE_DIR HULL_NO_CACHE HULL_NO_BUILD_CACHE 2>/dev/null || true
fail=0
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }
has() { grep -qF -- "$2" "$1"; }

rest="$WORK/rest_api_ts"
cli="$WORK/cli_modular_ts"
cp -R "$ROOT/examples/rest_api_ts" "$rest"
cp -R "$ROOT/examples/cli_modular_ts" "$cli"

# A build without the JavaScript frontend cannot run TypeScript.
if "$HULL" agent inspect "$cli" 2>/dev/null | grep -q '"status":"unsupported"'; then
    echo "e2e_typescript_examples: SKIP (no JavaScript frontend in this build)"
    exit 0
fi

echo "== typescript examples: hull types, the pipeline"
for d in "$rest" "$cli"; do
    "$HULL" types "$d" > "$WORK/types.txt" 2>&1 || { bad "hull types $d"; cat "$WORK/types.txt"; }
done
ls "$cli"/types/hull/*.d.ts > /dev/null 2>&1 && ok "hull types installed the declarations" ||
    bad "no declarations under types/hull"
# rest_api_ts: app.ts + tests/test_app.ts; cli_modular_ts: app.ts, two
# commands, two lib files, two tests. No .d.ts is a unit.
"$HULL" plugins run "$rest" > "$WORK/run_rest.txt" 2>&1 || true
"$HULL" plugins run "$cli" > "$WORK/run_cli.txt" 2>&1 || true
if has "$WORK/run_rest.txt" "committed  2 units" && has "$WORK/run_cli.txt" "committed  7 units" &&
   ! grep -q "d\.ts" "$WORK/run_rest.txt" "$WORK/run_cli.txt"; then
    ok "the erasure route takes the app's .ts files and no .d.ts"
else
    bad "the erasure route's units"; cat "$WORK/run_rest.txt" "$WORK/run_cli.txt"
fi

echo "== typescript examples: hull test"
"$HULL" test "$rest" > "$WORK/t_rest.txt" 2>&1 && has "$WORK/t_rest.txt" "10/10 tests passed" &&
    ok "rest_api_ts: hull test" || { bad "rest_api_ts: hull test"; cat "$WORK/t_rest.txt"; }
"$HULL" test "$cli" > "$WORK/t_cli.txt" 2>&1 && has "$WORK/t_cli.txt" "4/4 tests passed" &&
    ok "cli_modular_ts: hull test" || { bad "cli_modular_ts: hull test"; cat "$WORK/t_cli.txt"; }

echo "== typescript examples: run from source"
out=$(cd "$cli" && "$HULL" app.ts -- greet world 2>/dev/null) || true
[ "$out" = "hello world" ] && ok "cli_modular_ts: greet" || bad "greet printed '$out'"
out=$(cd "$cli" && "$HULL" app.ts -- count a b c 2>/dev/null) || true
[ "$out" = "3" ] && ok "cli_modular_ts: count" || bad "count printed '$out'"
rc=0; (cd "$cli" && "$HULL" app.ts -- nope) > "$WORK/nope.txt" 2>&1 || rc=$?
[ "$rc" -eq 1 ] && has "$WORK/nope.txt" "unknown command 'nope'" &&
    ok "cli_modular_ts: an unknown command exits 1" || { bad "unknown command (rc $rc)"; cat "$WORK/nope.txt"; }

port=$((20000 + $$ % 20000))
serve_check() {   # $1 = directory, $2 = command; checks the task routes on $port
    (cd "$1" && exec $2 -p "$port") > "$WORK/srv.log" 2>&1 &
    pid=$!
    n=0; until curl -s "http://127.0.0.1:$port/tasks" > /dev/null 2>&1 || [ $n -ge 50 ]; do
        sleep 0.1; n=$((n+1)); done
    curl -s -X POST -H 'Content-Type: application/json' -d '{"title":"Buy milk"}' \
        "http://127.0.0.1:$port/tasks" > "$WORK/post.json" 2>/dev/null || true
    curl -s -H 'Accept-Language: hu' "http://127.0.0.1:$port/tasks/99999" > "$WORK/404.json" 2>/dev/null || true
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
    has "$WORK/post.json" '"title":"Buy milk"' && has "$WORK/404.json" 'feladat nem tal'
}
serve_check "$rest" "$HULL app.ts" && ok "rest_api_ts: the routes answer, in the request's language" ||
    { bad "rest_api_ts served from source"; cat "$WORK/srv.log" "$WORK/post.json" "$WORK/404.json"; }

echo "== typescript examples: hull manifest, hull deploy"
"$HULL" manifest "$cli" > "$WORK/m_cli.txt" 2>&1 && has "$WORK/m_cli.txt" '{"modules":["hull/log@1"]}' &&
    ok "cli_modular_ts: hull manifest" || { bad "cli_modular_ts: hull manifest"; cat "$WORK/m_cli.txt"; }
"$HULL" manifest "$rest" > "$WORK/m_rest.txt" 2>&1 && has "$WORK/m_rest.txt" '"hull/i18n@1"' &&
    ok "rest_api_ts: hull manifest" || { bad "rest_api_ts: hull manifest"; cat "$WORK/m_rest.txt"; }
"$HULL" deploy dockerfile "$rest" > "$WORK/dep.txt" 2>&1 && [ -f "$rest/Dockerfile" ] &&
    ok "rest_api_ts: hull deploy dockerfile" || { bad "hull deploy"; cat "$WORK/dep.txt"; }
rm -f "$rest/Dockerfile" "$rest/.dockerignore"

echo "== typescript examples: hull build, sign, inspect, verify"
# Outputs go below $WORK, never into it: it is $HOME's parent, which a build
# may not write.
k="$WORK/keys"
mkdir -p "$k"
cp build/libhull_platform.a "$k/" 2>/dev/null || true
cp build/platform_canary_hash "$k/" 2>/dev/null || true
(cd "$k" && "$HULL" keygen > /dev/null 2>&1 && "$HULL" sign-platform --dir "$k/" "$k/developer" > /dev/null 2>&1) || true
rc=0; "$HULL" build --no-verify-platform --sign "$k/developer.key" --platform-sig "$k/platform.sig" \
    "$cli" -o "$cli/app" > "$WORK/b_cli.txt" 2>&1 || rc=$?
# A tree built without the platform library, or a hull with no bundled
# app_main.o or platform archives (a plain `make CC=cosmocc`), cannot link apps;
# a cosmo tree has no libhull_platform.a to sign-platform for --sign.
if [ "$rc" -ne 0 ] && { has "$WORK/b_cli.txt" "libhull_platform.a" ||
                        has "$WORK/b_cli.txt" "no bundled app_main.o" ||
                        has "$WORK/b_cli.txt" "cannot find platform archives" ||
                        has "$WORK/b_cli.txt" "cannot read platform.sig"; }; then
    echo "  SKIP: hull build (no platform library in this tree)"
else
    [ "$rc" -eq 0 ] && has "$WORK/b_cli.txt" "7 generated file(s) from the build pipeline" &&
        ok "cli_modular_ts: hull build embeds the 7 erased modules, no .d.ts" ||
        { bad "cli_modular_ts: hull build (rc $rc)"; cat "$WORK/b_cli.txt"; }
    mkdir -p "$WORK/nots"
    cp "$cli/app" "$WORK/nots/app" 2>/dev/null || true
    out=$(cd "$WORK/nots" && ./app greet built 2>/dev/null) || true
    [ "$out" = "hello built" ] && ok "cli_modular_ts: the binary runs with no .ts on disk" ||
        bad "built greet printed '$out'"
    "$HULL" inspect "$cli" > "$WORK/i_cli.txt" 2>&1 || true
    [ "$(grep -c 'Status:    VALID' "$WORK/i_cli.txt")" -eq 2 ] &&
        ok "cli_modular_ts: hull inspect reports both layers VALID" ||
        { bad "hull inspect"; cat "$WORK/i_cli.txt"; }
    "$HULL" verify --no-verify-platform --platform-key "$k/developer.pub" "$cli" > "$WORK/v_cli.txt" 2>&1 &&
        has "$WORK/v_cli.txt" "all checks passed" && ok "cli_modular_ts: hull verify" ||
        { bad "hull verify"; cat "$WORK/v_cli.txt"; }

    mkdir -p "$WORK/out" "$WORK/srv"
    rc=0; "$HULL" build --no-verify-platform "$rest" -o "$WORK/out/rest" > "$WORK/b_rest.txt" 2>&1 || rc=$?
    cp "$WORK/out/rest" "$WORK/srv/app" 2>/dev/null || true
    port=$((port + 1))
    [ "$rc" -eq 0 ] && serve_check "$WORK/srv" "./app" &&
        ok "rest_api_ts: the built server answers" ||
        { bad "rest_api_ts: built server (rc $rc)"; cat "$WORK/b_rest.txt" "$WORK/srv.log"; }
fi

[ "$fail" -eq 0 ] && echo "e2e_typescript_examples: OK" || { echo "e2e_typescript_examples: FAILED"; exit 1; }
