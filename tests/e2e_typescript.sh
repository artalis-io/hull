#!/bin/sh
# e2e_typescript.sh - TypeScript erasure through the build pipeline
# (docs/typescript_design.md §4.1, §4.2), through the real hull binary:
#   - the bundled stdlib:ts-erase@1 runs on the typescript route over every
#     analyzed .ts / .mts file: one generated output per file, named for the
#     file it shadows, its bytes the source with the types blanked (same
#     length, same lines);
#   - a second run is served from the build result store; an edit runs it
#     again; a JavaScript-only app does not run it;
#   - a construct the erasure refuses names its file and stops the
#     generation;
#   - `hull plugins list` shows the bundled plugin and its claim;
#   - the plugin's own tests pass under `hull test`, and the Lua stub host
#     answers erased_source as the JavaScript one does;
#   - a TypeScript app runs (docs/typescript_design.md §4.3, §4.4): `hull
#     app.ts`, `hull test` of tests/test_*.ts, `hull agent test`, a served
#     route, a stack trace naming the .ts file and line, `hull dev` keeping
#     its last good build across a refused edit; an entry beside another is
#     refused, as is a .ts module no pipeline erased;
#   - `hull build` embeds the erased modules: the binary runs with no .ts on
#     disk, --sign records each one's provenance, hull verify checks the
#     source against it, and the signed binary starts under --verify-sig.
#   - `hull new --runtime ts` scaffolds (flat, --cli, rest, cli) write .ts
#     sources, the declarations and a tsconfig.json, and pass hull test.
#   - `hull init --runtime ts` (server, --cli, --profile htmx) likewise, and
#     it refuses a TypeScript entry beside another one.
set -eu

HULL=${HULL:-build/hull}
case "$HULL" in /*) ;; *) HULL="$PWD/$HULL" ;; esac
PLUGIN="$PWD/stdlib/plugins/ts_erase"
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

d="$WORK/app"
mkdir -p "$d/lib"
cat > "$d/app.ts" <<'TS'
import { app } from "hull:app";
import { greet } from "./lib/a.ts";
interface Opts { name: string }
const o: Opts = { name: "x" };
app.manifest({ modules: [] });
app.main((ctx): number => { console.log(greet(o.name)); return 0; });
TS
cat > "$d/lib/a.ts" <<'TS'
export function greet(n: string): string { return "hi " + n; }
TS

# A build without the JavaScript frontend reports TypeScript unsupported.
if ! "$HULL" agent inspect "$d" 2>/dev/null | grep -q '"path":"app.ts","status":"analyzed"'; then
    if "$HULL" agent inspect "$d" 2>/dev/null | grep -q '"status":"unsupported"'; then
        echo "e2e_typescript: SKIP (no JavaScript frontend in this build)"
        exit 0
    fi
fi

echo "== typescript: the erasure route"
"$HULL" plugins run "$d" > "$WORK/run.txt" 2>&1 || true
if has "$WORK/run.txt" "stdlib:ts-erase@1  committed  2 units" &&
   has "$WORK/run.txt" "generated ./app.ts" && has "$WORK/run.txt" "generated ./lib/a.ts"; then
    ok "stdlib:ts-erase@1 committed one output per file"
else
    bad "the route run"; cat "$WORK/run.txt"
fi
"$HULL" plugins generation "$d" > "$WORK/gen.bin" 2> "$WORK/gen.err" || { bad "generation"; cat "$WORK/gen.err"; }
# The erased app.ts: the source with its types blanked, byte for byte (the
# bundle is a header line, its JSON, then each entry's bytes in order).
sed -e 's/^interface Opts { name: string }$/                               /' \
    -e 's/const o: Opts = /const o       = /' \
    -e 's/((ctx): number =>/((ctx)         =>/' "$d/app.ts" > "$WORK/want.js"
hdr=$(head -n 1 "$WORK/gen.bin")
jlen=${hdr##* }
skip=$(( ${#hdr} + 1 + jlen ))
tail -c +$((skip + 1)) "$WORK/gen.bin" | head -c "$(wc -c < "$d/app.ts")" > "$WORK/got.js"
cmp -s "$WORK/got.js" "$WORK/want.js" && ok "the generation carries the erased source, byte for byte" ||
    { bad "erased bytes"; cat "$WORK/got.js"; echo ---; cat "$WORK/want.js"; }
has "$WORK/gen.bin" '"vfs_name":"app.ts"' && has "$WORK/gen.bin" '"vfs_name":"lib/a.ts"' &&
    has "$WORK/gen.bin" '"position_map":"identity"' && ok "named for the files they shadow, positions kept" ||
    { bad "entry names"; cat "$WORK/gen.bin"; }
[ "$(wc -c < "$d/app.ts")" -gt 0 ] && (cd "$d" && find . -type f | sort) > "$WORK/files.txt"
[ "$(wc -l < "$WORK/files.txt")" -eq 2 ] && ok "nothing is written into the app" || { bad "app changed"; cat "$WORK/files.txt"; }

echo "== typescript: the result store"
"$HULL" plugins run "$d" > "$WORK/again.txt" 2>&1 || true
has "$WORK/again.txt" "stdlib:ts-erase@1  cached  2 units" && ok "a second run is served from the store" ||
    { bad "cache"; cat "$WORK/again.txt"; }
printf 'export const z: number = 3;\n' >> "$d/lib/a.ts"
"$HULL" plugins run "$d" > "$WORK/edit.txt" 2>&1 || true
has "$WORK/edit.txt" "stdlib:ts-erase@1  committed  2 units" && ok "an edit runs it again" ||
    { bad "edit"; cat "$WORK/edit.txt"; }

echo "== typescript: refusals"
cp "$d/lib/a.ts" "$WORK/a.ts.good"
printf 'enum E { A }\n' >> "$d/lib/a.ts"
rc=0; "$HULL" plugins generation "$d" > /dev/null 2> "$WORK/enum.txt" || rc=$?
[ "$rc" -ne 0 ] && has "$WORK/enum.txt" "lib/a.ts:3:1: ts.unsupported: TypeScript enums are not supported" &&
    ok "a refused construct names its file and position, and stops the generation" ||
    { bad "enum refusal (rc $rc)"; cat "$WORK/enum.txt"; }
cp "$WORK/a.ts.good" "$d/lib/a.ts"
# U+2028 inside type syntax: the discovery is valid, the erasure refuses it.
printf 'let q: /* \342\200\250 */ number = 1;\n' >> "$d/lib/a.ts"
rc=0; "$HULL" plugins run "$d" > "$WORK/ls.txt" 2>&1 || rc=$?
[ "$rc" -eq 1 ] && has "$WORK/ls.txt" "stdlib:ts-erase@1  failed  2 units  diagnostic_error" &&
    has "$WORK/ls.txt" "error ts.unsupported: lib/a.ts: a line separator" &&
    ok "a refusal from the erasure is the plugin's error diagnostic" || { bad "erasure refusal (rc $rc)"; cat "$WORK/ls.txt"; }
cp "$WORK/a.ts.good" "$d/lib/a.ts"

echo "== typescript: no TypeScript"
j="$WORK/js"
mkdir -p "$j"
printf 'import { app } from "hull:app";\napp.manifest({ modules: [] });\napp.main(() => 0);\n' > "$j/app.js"
"$HULL" plugins run "$j" > "$WORK/js.txt" 2>&1 || true
has "$WORK/js.txt" "no build plugins" && ok "a JavaScript app does not run the route" || { bad "js app"; cat "$WORK/js.txt"; }
"$HULL" plugins run "$j" --json > "$WORK/js.json" 2>&1 || true
has "$WORK/js.json" '"status":"no_input"' && ok "--json reports the route with no input" || { bad "js json"; cat "$WORK/js.json"; }

echo "== typescript: list, the plugin's tests"
"$HULL" plugins list "$d" > "$WORK/list.txt" 2>&1 || true
has "$WORK/list.txt" "stdlib:ts-erase@1" && has "$WORK/list.txt" "javascript (bundled), source -> generated" &&
    has "$WORK/list.txt" "claims: module:typescript" && ok "list shows the bundled plugin on its route" ||
    { bad "list"; cat "$WORK/list.txt"; }
"$HULL" test "$PLUGIN" > "$WORK/ptest.txt" 2>&1 && has "$WORK/ptest.txt" "3/3 tests passed" &&
    ok "the plugin's own tests pass" || { bad "plugin tests"; cat "$WORK/ptest.txt"; }

# The same plugin written in Lua, against the Lua stub host: the same answers.
l="$WORK/lua_erase"
mkdir -p "$l/tests"
cat > "$l/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/build-plugin@1" },
    build_plugin = { id = "lua-erase", version = 1, input = "source", languages = { "typescript" },
                     output = "generated" },
})
app.transform(function(ctx)
    local refused = false
    for _, u in ipairs(assert(ctx.source.units())) do
        local r = assert(ctx.source.erased_source(u))
        if #r.refusals > 0 then
            for _, d in ipairs(r.refusals) do assert(ctx.diagnostics.emit(d)) end
            refused = true
        elseif not refused then
            local w = assert(ctx.outputs.create_blob(u.id))
            assert(w:write(r.bytes))
            assert(w:close())
            assert(ctx.outputs.declare_generated_output({ name = u.name, position_map = "identity" }, w))
        end
    end
end)
LUA
cat > "$l/tests/test_lua_erase.lua" <<'LUA'
local U = { id = "module:typescript:app.ts", language = "typescript", path = "app.ts", kind = "module",
            name = "./app.ts", sha256 = string.rep("0", 64) }
test("the erased bytes are the output", function()
    local r = test.run_transform({ units = { U }, erased = { [U.id] = "const x         = 1;\n" } })
    test.eq(r.error, nil)
    test.eq(#r.outputs, 1)
    test.eq(r.outputs[1].descriptor.name, "./app.ts")
    test.eq(r.outputs[1].bytes, "const x         = 1;\n")
end)
test("a refusal is a diagnostic", function()
    local d = { severity = "error", code = "ts.unsupported", message = "m", path = "app.ts" }
    local r = test.run_transform({ units = { U }, erased = { [U.id] = { refusals = { d } } } })
    test.eq(#r.outputs, 0)
    test.eq(r.diagnostics[1].code, "ts.unsupported")
end)
test("a unit the stub cannot erase", function()
    local r = test.run_transform({ units = { U } })
    test.ok(r.error and r.error:find("unsupported_op", 1, true), tostring(r.error))
end)
LUA
"$HULL" test "$l" > "$WORK/ltest.txt" 2>&1 && has "$WORK/ltest.txt" "3/3 tests passed" &&
    ok "the Lua stub host answers erased_source the same way" || { bad "lua stub"; cat "$WORK/ltest.txt"; }

echo "== typescript: running an app"
out=$(cd "$d" && "$HULL" app.ts 2>&1) || true
case "$out" in *"hi x"*) ok "hull app.ts runs the erased entry and its .ts import" ;;
    *) bad "hull app.ts"; echo "$out" ;; esac

s="$WORK/srv"
mkdir -p "$s/lib" "$s/tests"
cat > "$s/app.ts" <<'TS'
import { app } from "hull:app";
import { double, boom } from "./lib/a.ts";
type Reply = { n: number };
app.manifest({ modules: ["hull/http-server@1"] });
app.get("/n", (_req: unknown, res: any) => { const r: Reply = { n: double(21) }; res.json(r); });
app.get("/boom", (_req: unknown, res: any) => { res.json({ v: boom() }); });
TS
cat > "$s/lib/a.ts" <<'TS'
export function double(n: number): number { return n * 2; }
export function boom(): number {
  const why: string = "typed";
  throw new Error("boom " + why);
}
TS
cat > "$s/tests/test_app.ts" <<'TS'
import { double } from "../lib/a.ts";
test("double", () => { const v: number = double(4); test.eq(v, 8); });
test("route", () => { const r = test.get("/n"); test.eq(r.status, 200); test.eq(r.json.n, 42); });
TS
# Run from the app directory: a test file's relative import does not resolve
# yet when the app directory is named through a symbolic link (mktemp's /var
# on macOS), a JavaScript test's included.
(cd "$s" && "$HULL" test .) > "$WORK/stest.txt" 2>&1 && has "$WORK/stest.txt" "test_app.ts" &&
    has "$WORK/stest.txt" "2/2 tests passed" && ok "hull test runs tests/test_*.ts" ||
    { bad "hull test"; cat "$WORK/stest.txt"; }
(cd "$s" && "$HULL" agent test .) > "$WORK/atest.json" 2>&1 &&
    has "$WORK/atest.json" '"name":"test_app.ts"' && has "$WORK/atest.json" '"passed":2' &&
    ok "hull agent test runs them too" || { bad "agent test"; cat "$WORK/atest.json"; }

port=$((20000 + $$ % 20000))
(cd "$s" && exec "$HULL" app.ts -p "$port") > "$WORK/srv.log" 2>&1 &
pid=$!
n=0; until curl -s "http://127.0.0.1:$port/n" > "$WORK/n.json" 2>/dev/null || [ $n -ge 50 ]; do sleep 0.1; n=$((n+1)); done
has "$WORK/n.json" '{"n":42}' && ok "a served route answers" || { bad "serve"; cat "$WORK/srv.log"; }
curl -s "http://127.0.0.1:$port/boom" > /dev/null 2>&1 || true
sleep 0.3
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
has "$WORK/srv.log" "lib/a.ts:4:" && ok "a stack trace names the .ts file and line" ||
    { bad "stack trace"; cat "$WORK/srv.log"; }

port=$((port + 1))
(cd "$s" && exec "$HULL" dev -p "$port") > "$WORK/dev.log" 2>&1 &
pid=$!
n=0; until curl -s "http://127.0.0.1:$port/n" > "$WORK/d1.json" 2>/dev/null || [ $n -ge 100 ]; do sleep 0.1; n=$((n+1)); done
cp "$s/lib/a.ts" "$WORK/sa.good"
# The watcher compares modification times in whole seconds: each edit comes
# a second after the state it changes.
sleep 1.1
printf 'enum E { A }\n' >> "$s/lib/a.ts"
n=0; until has "$WORK/dev.log" "keeps running its last good build" || [ $n -ge 100 ]; do sleep 0.1; n=$((n+1)); done
curl -s "http://127.0.0.1:$port/n" > "$WORK/d2.json" 2>/dev/null || true
sleep 1.1
sed 's/n \* 2/n * 3/' "$WORK/sa.good" > "$s/lib/a.ts"
n=0; until { curl -s "http://127.0.0.1:$port/n" > "$WORK/d3.json" 2>/dev/null && has "$WORK/d3.json" '63'; } || [ $n -ge 100 ]; do sleep 0.1; n=$((n+1)); done
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
cp "$WORK/sa.good" "$s/lib/a.ts"
has "$WORK/d1.json" '{"n":42}' && has "$WORK/dev.log" "lib/a.ts:6:1: ts.unsupported" &&
    has "$WORK/d2.json" '{"n":42}' && has "$WORK/d3.json" '{"n":63}' &&
    ok "hull dev keeps its last good build across a refused edit, and a fix reloads" ||
    { bad "hull dev"; cat "$WORK/dev.log"; cat "$WORK/d1.json" "$WORK/d2.json" "$WORK/d3.json"; }

echo "== typescript: hull new --runtime ts"
n="$WORK/new"
mkdir -p "$n"
for spec in "flat:" "cliflat:--cli" "rest:--type rest" "cli:--type cli"; do
    name=${spec%%:*}; args=${spec#*:}
    # shellcheck disable=SC2086
    (cd "$n" && "$HULL" new "$name" --runtime ts $args) > "$WORK/new_$name.txt" 2>&1 ||
        { bad "hull new $name"; cat "$WORK/new_$name.txt"; continue; }
    if [ -f "$n/$name/app.ts" ] && [ ! -e "$n/$name/app.js" ] &&
       [ -f "$n/$name/types/hull/.hull-types" ] && [ -f "$n/$name/tsconfig.json" ] &&
       ! find "$n/$name" -name '*.js' -not -path '*/types/*' | grep -q .; then
        ok "hull new $name --runtime ts writes .ts sources, types/ and tsconfig.json"
    else
        bad "hull new $name layout"; find "$n/$name" -type f | sort
    fi
    (cd "$n/$name" && "$HULL" test) > "$WORK/newt_$name.txt" 2>&1 &&
        has "$WORK/newt_$name.txt" "tests passed" && ! has "$WORK/newt_$name.txt" "FAIL" &&
        ok "the $name TypeScript scaffold passes hull test" ||
        { bad "hull test $name"; cat "$WORK/newt_$name.txt"; }
    "$HULL" types "$n/$name" --check > "$WORK/newc_$name.txt" 2>&1 &&
        ok "the $name scaffold's declarations are this hull's" ||
        { bad "hull types --check $name"; cat "$WORK/newc_$name.txt"; }
done
rc=0; (cd "$n/cli" && "$HULL" app.ts -- nope) > "$WORK/newu.txt" 2>&1 || rc=$?
[ "$rc" -eq 1 ] && has "$WORK/newu.txt" "unknown command 'nope'" &&
    ok "an import of a .ts file that does not exist is 'module not found'" ||
    { bad "unknown command (rc $rc)"; cat "$WORK/newu.txt"; }
(cd "$n/cliflat" && "$HULL" app.ts -- ada) > "$WORK/newr.txt" 2>&1 && has "$WORK/newr.txt" "hello ada" &&
    ok "the flat CLI TypeScript scaffold runs" || { bad "cliflat run"; cat "$WORK/newr.txt"; }
rc=0; (cd "$n" && "$HULL" new tui --runtime ts --type tui) > "$WORK/newtui.txt" 2>&1 || rc=$?
[ "$rc" -ne 0 ] && has "$WORK/newtui.txt" "the TUI layout is Lua-only" &&
    ok "hull new --type tui --runtime ts is refused" || { bad "tui (rc $rc)"; cat "$WORK/newtui.txt"; }
echo "== typescript: hull init --runtime ts"
for spec in "isrv:" "icli:--cli" "ihx:--profile htmx"; do
    name=${spec%%:*}; args=${spec#*:}
    # shellcheck disable=SC2086
    (cd "$n" && "$HULL" init "$name" --runtime ts $args) > "$WORK/init_$name.txt" 2>&1 ||
        { bad "hull init $name"; cat "$WORK/init_$name.txt"; continue; }
    [ -f "$n/$name/app.ts" ] && [ -f "$n/$name/tests/test_app.ts" ] &&
        [ -f "$n/$name/types/hull/.hull-types" ] && [ -f "$n/$name/tsconfig.json" ] &&
        ok "hull init $name --runtime ts writes app.ts, types/ and tsconfig.json" ||
        { bad "hull init $name layout"; find "$n/$name" -type f | sort; }
    (cd "$n/$name" && "$HULL" test) > "$WORK/initt_$name.txt" 2>&1 &&
        has "$WORK/initt_$name.txt" "tests passed" && ! has "$WORK/initt_$name.txt" "FAIL" &&
        ok "the $name TypeScript init scaffold passes hull test" ||
        { bad "hull test $name"; cat "$WORK/initt_$name.txt"; }
done
(cd "$n" && "$HULL" init isrv) > "$WORK/init_re.txt" 2>&1 &&
    has "$WORK/init_re.txt" "runtime   ts" && has "$WORK/init_re.txt" "isrv/types/" &&
    ! has "$WORK/init_re.txt" "created:" &&
    ok "hull init again detects app.ts and changes nothing" || { bad "init rerun"; cat "$WORK/init_re.txt"; }
mkdir -p "$n/imts" && printf 'import { app } from "hull:app";\napp.manifest({ modules: [] });\napp.main(() => 0);\n' > "$n/imts/app.mts"
(cd "$n" && "$HULL" init imts) > "$WORK/init_mts.txt" 2>&1 && [ ! -e "$n/imts/app.ts" ] &&
    has "$WORK/init_mts.txt" "runtime   ts" &&
    ok "hull init takes an app.mts as the TypeScript entry" || { bad "init app.mts"; cat "$WORK/init_mts.txt"; }
mkdir -p "$n/ilua" && : > "$n/ilua/app.lua"
rc=0; (cd "$n" && "$HULL" init ilua --runtime ts) > "$WORK/init_amb.txt" 2>&1 || rc=$?
[ "$rc" -ne 0 ] && [ ! -e "$n/ilua/app.ts" ] && has "$WORK/init_amb.txt" "already has app.lua" &&
    ok "hull init refuses a TypeScript entry beside app.lua" || { bad "init ambiguity (rc $rc)"; cat "$WORK/init_amb.txt"; }
mkdir -p "$n/its" && : > "$n/its/app.ts"
rc=0; (cd "$n" && "$HULL" init its --runtime js) > "$WORK/init_amb2.txt" 2>&1 || rc=$?
[ "$rc" -ne 0 ] && [ ! -e "$n/its/app.js" ] && has "$WORK/init_amb2.txt" "already has app.ts" &&
    ok "hull init refuses a JavaScript entry beside app.ts" || { bad "init ambiguity 2 (rc $rc)"; cat "$WORK/init_amb2.txt"; }
rc=0; (cd "$n" && "$HULL" new bad --runtime py) > "$WORK/newbad.txt" 2>&1 || rc=$?
[ "$rc" -ne 0 ] && has "$WORK/newbad.txt" "use lua, js or ts" &&
    ok "an unknown runtime names the three" || { bad "bad runtime (rc $rc)"; cat "$WORK/newbad.txt"; }
echo "== typescript: an app below a build/ directory"
u="$WORK/build/vendor/app"
mkdir -p "$u"
printf 'import { app } from "hull:app";\nimport { d } from "./lib.ts";\napp.manifest({ modules: [] });\napp.main((ctx) => { ctx.stdout.write("v" + d(2) + "\\n"); return 0; });\n' > "$u/app.ts"
printf 'export function d(n: number): number { return n * 2; }\n' > "$u/lib.ts"
(cd "$u" && "$HULL" app.ts) > "$WORK/under.txt" 2>&1 && has "$WORK/under.txt" "v4" &&
    ok "a TypeScript app whose path has a build/ directory is erased and runs" ||
    { bad "app below build/"; cat "$WORK/under.txt"; }

echo "== typescript: entries refused"
: > "$s/app.js"
rc=0; "$HULL" test "$s" > "$WORK/amb.txt" 2>&1 || rc=$?
[ "$rc" -ne 0 ] && has "$WORK/amb.txt" "has both app.ts and app.js" &&
    ok "an app.ts beside app.js is ambiguous" || { bad "ambiguity (rc $rc)"; cat "$WORK/amb.txt"; }
rm -f "$s/app.js"
printf 'export function d(n: number): number { return n; }\n' > "$j/b.ts"
printf 'import { app } from "hull:app";\nimport { d } from "./b.ts";\napp.manifest({ modules: [] });\napp.main(() => d(0));\n' > "$j/app.js"
rc=0; (cd "$j" && "$HULL" app.js) > "$WORK/jts.txt" 2>&1 || rc=$?
[ "$rc" -ne 0 ] && has "$WORK/jts.txt" "is TypeScript and was not erased by the build pipeline" &&
    ok "a .ts module no pipeline erased is refused, never run as JavaScript" ||
    { bad "unerased .ts (rc $rc)"; cat "$WORK/jts.txt"; }
echo "== typescript: hull build"
# A built binary runs the erased modules embedded under their source names,
# with no TypeScript on disk; --sign records each one's provenance, and hull
# verify checks the source against it. Needs the platform library.
# Outputs go below $WORK: it is $HOME's parent, which a build may not write.
mkdir -p "$WORK/out"
rc=0; "$HULL" build --no-verify-platform "$d" -o "$WORK/out/bin_main" > "$WORK/b1.txt" 2>&1 || rc=$?
# A tree built without the platform library, or a hull with no bundled
# app_main.o or platform archives (a plain `make CC=cosmocc`), cannot link apps.
if [ "$rc" -ne 0 ] && { has "$WORK/b1.txt" "libhull_platform.a" ||
                        has "$WORK/b1.txt" "no bundled app_main.o" ||
                        has "$WORK/b1.txt" "cannot find platform archives"; }; then
    echo "  SKIP: hull build (no platform library in this tree)"
else
    mkdir -p "$WORK/run1"
    cp "$WORK/out/bin_main" "$WORK/run1/app" 2>/dev/null || true
    out=$(cd "$WORK/run1" && ./app 2>&1) || true
    [ "$rc" -eq 0 ] && has "$WORK/b1.txt" "2 generated file(s) from the build pipeline" &&
        case "$out" in *"hi x"*) true ;; *) false ;; esac &&
        ok "a built app.main TypeScript app runs with no .ts on disk" ||
        { bad "built app.main (rc $rc)"; cat "$WORK/b1.txt"; echo "$out"; }

    rc=0; "$HULL" build --no-verify-platform "$s" -o "$WORK/out/bin_srv" > "$WORK/b2.txt" 2>&1 || rc=$?
    mkdir -p "$WORK/run2"
    cp "$WORK/out/bin_srv" "$WORK/run2/app" 2>/dev/null || true
    port=$((port + 1))
    (cd "$WORK/run2" && exec ./app -p "$port") > "$WORK/run2.log" 2>&1 &
    pid=$!
    n=0; until curl -s "http://127.0.0.1:$port/n" > "$WORK/bn.json" 2>/dev/null || [ $n -ge 50 ]; do sleep 0.1; n=$((n+1)); done
    curl -s "http://127.0.0.1:$port/boom" > /dev/null 2>&1 || true
    sleep 0.3
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
    [ "$rc" -eq 0 ] && has "$WORK/bn.json" '{"n":42}' && has "$WORK/run2.log" "lib/a.ts:4:" &&
        ok "a built server answers, its stack trace naming the .ts file and line" ||
        { bad "built server (rc $rc)"; cat "$WORK/b2.txt" "$WORK/run2.log"; }

    k="$WORK/keys"
    mkdir -p "$k"
    cp build/libhull_platform.a "$k/" 2>/dev/null || true
    cp build/platform_canary_hash "$k/" 2>/dev/null || true
    (cd "$k" && "$HULL" keygen > /dev/null 2>&1 && "$HULL" sign-platform --dir "$k/" "$k/developer" > /dev/null 2>&1) || true
    rc=0; "$HULL" build --no-verify-platform --sign "$k/developer.key" --platform-sig "$k/platform.sig" \
        "$s" -o "$s/app" > "$WORK/b3.txt" 2>&1 || rc=$?
    if [ "$rc" -eq 0 ] && has "$s/package.sig" '"generated":{"app.ts":{"key":"' &&
       has "$s/package.sig" '"plugin":"stdlib:ts-erase@1","source":"'; then
        ok "--sign records each generated file's plugin, action key and source hash"
    else
        bad "sign (rc $rc)"; cat "$WORK/b3.txt"; cat "$s/package.sig" 2>/dev/null
    fi
    "$HULL" verify --no-verify-platform --platform-key "$k/developer.pub" "$s" > "$WORK/v1.txt" 2>&1 &&
        has "$WORK/v1.txt" "all checks passed" && ok "hull verify checks the source against the recorded hash" ||
        { bad "verify"; cat "$WORK/v1.txt"; }
    port=$((port + 1))
    (cd "$s" && exec ./app --verify-sig "$k/developer.pub" --hull-no-verify-platform -p "$port") > "$WORK/vs.log" 2>&1 &
    pid=$!
    n=0; until curl -s "http://127.0.0.1:$port/n" > "$WORK/vs.json" 2>/dev/null || [ $n -ge 50 ]; do sleep 0.1; n=$((n+1)); done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
    has "$WORK/vs.json" '{"n":42}' && ok "the signed binary starts under --verify-sig" ||
        { bad "--verify-sig"; cat "$WORK/vs.log"; }
    printf '// changed\n' >> "$s/lib/a.ts"
    rc=0; "$HULL" verify --no-verify-platform --platform-key "$k/developer.pub" "$s" > "$WORK/v2.txt" 2>&1 || rc=$?
    cp "$WORK/sa.good" "$s/lib/a.ts"
    [ "$rc" -ne 0 ] && has "$WORK/v2.txt" "lib/a.ts" && ok "an edited .ts source fails hull verify" ||
        { bad "verify tamper (rc $rc)"; cat "$WORK/v2.txt"; }
fi

[ "$fail" -eq 0 ] && echo "e2e_typescript: OK" || { echo "e2e_typescript: FAILED"; exit 1; }
