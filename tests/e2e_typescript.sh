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
#     answers erased_source as the JavaScript one does.
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

[ "$fail" -eq 0 ] && echo "e2e_typescript: OK" || { echo "e2e_typescript: FAILED"; exit 1; }
