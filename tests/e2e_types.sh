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

sha() { h=$(shasum -a 256 "$1" 2>/dev/null || sha256sum "$1"); echo "${h%% *}"; }

echo "── types_audit46.record_names: a planted record cannot delete outside types/ ──"
S=$WORK/a46; mkdir -p "$S/victim/vcs" "$S/evil/types/hull"
printf 'ref: refs/heads/main\n' > "$S/victim/vcs/HEAD"; echo x > "$S/evil/app.js"
printf 'version 0\n%s ../../victim/vcs/HEAD\n' "$(sha "$S/victim/vcs/HEAD")" > "$S/evil/types/hull/.hull-types"
out=$(cd "$S/victim" && "$HULL" types ../evil 2>&1)
check "a '../' record entry deletes nothing" "[ -f '$S/victim/vcs/HEAD' ]"
check "nothing is reported removed" "echo \"\$out\" | grep -q ' 0 removed'"
printf 'version 0\n%s ../../victim/vcs/HEAD\n' "$(sha "$S/victim/vcs/HEAD")" >> "$S/evil/types/hull/.hull-types"
out=$(cd "$S/victim" && "$HULL" types --check ../evil 2>&1); rc=$?
check "--check reports the foreign entry" "[ $rc -ne 0 ] && echo \"\$out\" | grep -q 'ignored'"
check "--check deleted nothing either" "[ -f '$S/victim/vcs/HEAD' ]"

echo "── types_audit46.symlinked_dir: no write or removal through a linked directory ──"
# The link targets are in the app tree, where the tool sandbox lets hull
# types write: only the link check stops it.
A3=$WORK/a46b; mkdir -p "$A3/src"; echo x > "$A3/app.js"
(cd "$A3" && "$HULL" types >/dev/null 2>&1)
cp "$A3"/types/hull/modules/*.d.ts "$A3/src/"
printf '\n// my own source\n' >> "$A3/src/db.d.ts"
rm -rf "$A3/types/hull/modules"; ln -s ../../src "$A3/types/hull/modules"
out=$(cd "$A3" && "$HULL" types --force 2>&1); rc=$?
check "a symlinked types/hull/modules is refused, even with --force" \
      "[ $rc -ne 0 ] && grep -q 'my own source' '$A3/src/db.d.ts'"
check "the link is named, with no --force advice" \
      "echo \"\$out\" | grep -q 'symbolic link' && ! echo \"\$out\" | grep -q 'Re-run with --force'"
# A recorded stale file reached through a linked directory is not removed.
rm "$A3/types/hull/modules"; (cd "$A3" && "$HULL" types >/dev/null 2>&1)
mkdir -p "$A3/old"; printf 'declare const gone: 1;\n' > "$A3/old/gone.d.ts"
ln -s ../../old "$A3/types/hull/old"
printf '%s hull/old/gone.d.ts\n' "$(sha "$A3/old/gone.d.ts")" >> "$A3/types/hull/.hull-types"
(cd "$A3" && "$HULL" types >/dev/null 2>&1); rc=$?
check "a stale file behind a linked directory is kept (and refused)" "[ $rc -ne 0 ] && [ -f '$A3/old/gone.d.ts' ]"
rm "$A3/types/hull/old"
# A symlinked tests/ gets no tsconfig.json.
A4=$WORK/a46c; mkdir -p "$A4/other"; echo x > "$A4/app.js"; ln -s other "$A4/tests"
(cd "$A4" && "$HULL" types >/dev/null 2>&1)
check "no tsconfig.json written into a linked tests/" "[ ! -f '$A4/other/tsconfig.json' ]"

echo "── types_audit46.removed_count: a refused removal is not counted ──"
A5=$WORK/a46d; mkdir -p "$A5"; echo x > "$A5/app.js"
(cd "$A5" && "$HULL" types >/dev/null 2>&1)
mkdir -p "$A5/types/hull/ro"; printf 'declare const gone: 1;\n' > "$A5/types/hull/ro/gone.d.ts"
printf '%s hull/ro/gone.d.ts\n' "$(sha "$A5/types/hull/ro/gone.d.ts")" >> "$A5/types/hull/.hull-types"
chmod 555 "$A5/types/hull/ro"
out=$(cd "$A5" && "$HULL" types 2>&1)
if [ -f "$A5/types/hull/ro/gone.d.ts" ]; then
    check "a failed removal is not counted" "echo \"\$out\" | grep -q ' 0 removed'"
    check "a failed removal is reported" "echo \"\$out\" | grep -q 'could not remove'"
    check "a failed removal stays in the record" "grep -q 'hull/ro/gone.d.ts' '$A5/types/hull/.hull-types'"
    chmod 755 "$A5/types/hull/ro"
    out=$(cd "$A5" && "$HULL" types 2>&1)
    check "the next run removes it" "[ ! -f '$A5/types/hull/ro/gone.d.ts' ] && echo \"\$out\" | grep -q ' 1 removed'"
else
    echo "  skip  removal count (running as root)"
fi
chmod 755 "$A5/types/hull/ro"

echo "── types_audit47.case_variant: a case variant of a shipped name is not stale ──"
A6=$WORK/a47a; mkdir -p "$A6"; echo x > "$A6/app.js"
(cd "$A6" && "$HULL" types >/dev/null 2>&1)
printf '%s hull/Core.d.ts\n' "$(sha "$A6/types/hull/core.d.ts")" >> "$A6/types/hull/.hull-types"
out=$(cd "$A6" && "$HULL" types 2>&1)
check "a case-variant record entry removes no shipped file" \
      "[ -f '$A6/types/hull/core.d.ts' ] && echo \"\$out\" | grep -q ' 0 removed'"

echo "── types_audit47.fifo: a planted FIFO does not hang hull types ──"
# Run each in the background and kill it after 10 s: the pre-fix open waited
# for a writer / reader for ever.
# The command is backgrounded directly and each sh -c execs hull, so $bp is
# hull itself and the kill reaches it.
bounded() { "$@" >/dev/null 2>&1 & bp=$!; i=0
    while kill -0 $bp 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    if kill -0 $bp 2>/dev/null; then kill -9 $bp 2>/dev/null; wait $bp 2>/dev/null; return 124; fi
    wait $bp; return 0; }
A7=$WORK/a47b; mkdir -p "$A7"; echo x > "$A7/app.js"
(cd "$A7" && "$HULL" types >/dev/null 2>&1)
if mkfifo "$A7/types/hull/old.d.ts" 2>/dev/null; then
    printf '%s hull/old.d.ts\n' "$(sha "$A7/types/hull/core.d.ts")" >> "$A7/types/hull/.hull-types"
    bounded sh -c "cd '$A7' && exec '$HULL' types --check"; rc=$?
    check "--check with a recorded FIFO finishes" "[ $rc -ne 124 ]"
    rm "$A7/types/hull/old.d.ts"
    rm "$A7/types/hull/modules/db.d.ts"; mkfifo "$A7/types/hull/modules/db.d.ts"
    bounded sh -c "cd '$A7' && exec '$HULL' types"; rc=$?
    check "a FIFO at a shipped name does not hang a run" "[ $rc -ne 124 ]"
    bounded sh -c "cd '$A7' && exec '$HULL' types --force"; rc=$?
    check "a FIFO at a shipped name does not hang --force" "[ $rc -ne 124 ]"
else
    echo "  skip  fifo (mkfifo unavailable)"
fi

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
