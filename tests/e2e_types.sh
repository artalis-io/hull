#!/bin/sh
# E2E for `hull types`: installs the embedded TypeScript declarations into an
# app, re-runs idempotently, refuses to replace an edited file without --force,
# --check reports drift, a removed file is cleaned up, an existing tsconfig is
# left alone, a symlinked destination is refused, and - with the pinned
# TypeScript (tests/types) - an app type-checks against what was installed:
# a correct call passes and a wrong one fails.
#
#   sh tests/e2e_types.sh            (make e2e-types)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HULL=${HULL:-$ROOT/build/hull}
TSC=$ROOT/tests/types/node_modules/.bin/tsc
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0
ok()  { echo "  ok  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL $1"; fail=$((fail + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

APP=$WORK/app
mkdir -p "$APP/tests"
cat > "$APP/app.js" <<'JS'
import { app } from "hull:app";
import { db as dbModule } from "hull:db";
app.manifest({ modules: ["hull/http-server@1", "hull/db@1"] });
app.get("/n", (req, res) => {
    const rows = dbModule.default().query("SELECT 1 AS n");
    res.json({ rows, path: req.path });
});
JS

echo "── hull types: install ──"
out=$(cd "$APP" && "$HULL" types 2>&1); rc=$?
check "first run exits 0" "[ $rc -eq 0 ]"
check "core.d.ts installed" "[ -f '$APP/types/hull/core.d.ts' ]"
check "a module declaration installed" "[ -f '$APP/types/hull/modules/db.d.ts' ]"
check "record written" "[ -f '$APP/types/hull/.hull-types' ]"
check "tsconfig.json created" "[ -f '$APP/tsconfig.json' ]"
check "tests/tsconfig.json created" "[ -f '$APP/tests/tsconfig.json' ]"

echo "── idempotent + --check ──"
out=$(cd "$APP" && "$HULL" types 2>&1); rc=$?
check "second run exits 0" "[ $rc -eq 0 ]"
check "second run writes nothing" "echo \"\$out\" | grep -q ' 0 written'"
(cd "$APP" && "$HULL" types --check >/dev/null 2>&1); rc=$?
check "--check passes on a fresh install" "[ $rc -eq 0 ]"

echo "── edited file ──"
printf '\n// local edit\n' >> "$APP/types/hull/modules/db.d.ts"
(cd "$APP" && "$HULL" types --check >/dev/null 2>&1); rc=$?
check "--check fails after an edit" "[ $rc -ne 0 ]"
out=$(cd "$APP" && "$HULL" types 2>&1); rc=$?
check "an edited file is a conflict (exit 1)" "[ $rc -ne 0 ]"
check "the conflict names the file" "echo \"\$out\" | grep -q 'modules/db.d.ts'"
check "the edit is kept" "grep -q 'local edit' '$APP/types/hull/modules/db.d.ts'"
(cd "$APP" && "$HULL" types --force >/dev/null 2>&1); rc=$?
check "--force replaces it" "[ $rc -eq 0 ] && ! grep -q 'local edit' '$APP/types/hull/modules/db.d.ts'"

echo "── a file this version no longer ships ──"
printf 'declare const gone: 1;\n' > "$APP/types/hull/modules/gone.d.ts"
h=$(shasum -a 256 "$APP/types/hull/modules/gone.d.ts" 2>/dev/null || sha256sum "$APP/types/hull/modules/gone.d.ts")
printf '%s hull/modules/gone.d.ts\n' "${h%% *}" >> "$APP/types/hull/.hull-types"
(cd "$APP" && "$HULL" types >/dev/null 2>&1)
check "a recorded, unchanged, no-longer-shipped file is removed" "[ ! -f '$APP/types/hull/modules/gone.d.ts' ]"

echo "── existing tsconfig ──"
printf '{ "custom": true }\n' > "$APP/tsconfig.json"
(cd "$APP" && "$HULL" types >/dev/null 2>&1)
check "an existing tsconfig.json is left alone" "grep -q custom '$APP/tsconfig.json'"
rm "$APP/tsconfig.json"; (cd "$APP" && "$HULL" types >/dev/null 2>&1)

echo "── symlinked destination ──"
APP2=$WORK/app2; mkdir -p "$APP2" "$WORK/elsewhere"; ln -s "$WORK/elsewhere" "$APP2/types"
(cd "$APP2" && "$HULL" types >/dev/null 2>&1); rc=$?
check "a symlinked types/ is refused" "[ $rc -ne 0 ] && [ -z \"\$(ls '$WORK/elsewhere')\" ]"

echo "── type-check with the installed declarations ──"
if [ -x "$TSC" ]; then
    out=$("$TSC" --noEmit -p "$APP" 2>&1); rc=$?
    check "a correct app type-checks" "[ $rc -eq 0 ]"
    [ $rc -eq 0 ] || echo "$out" | head -20
    cat > "$APP/bad.js" <<'JS'
import { db as dbModule } from "hull:db";
dbModule.default().query(42);
JS
    out=$("$TSC" --noEmit -p "$APP" 2>&1); rc=$?
    check "a wrong call fails the type-check" "[ $rc -ne 0 ] && echo \"\$out\" | grep -q 'bad.js'"
    rm "$APP/bad.js"
else
    echo "  skip  type-check (run: cd tests/types && npm ci)"
fi

echo
echo "e2e_types: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
