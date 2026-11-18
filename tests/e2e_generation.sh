#!/bin/sh
# e2e_generation.sh - a pipeline generation handed to an app run from source
# (docs/dev_pipeline_generations_design.md §9.1), built here by
# hand over fixture payloads (no plugin runs):
#   - the entry, a module, a module that exists only in the generation and a
#     template load from the generation, never from the files they shadow,
#     in Lua and in JavaScript, for `hull app.*` and for `hull test`;
#   - a payload that does not match its hash, a bad name, and a descriptor
#     that is not one stop the app before any code runs - the shadowed
#     sources never run;
#   - a generation-shaped file planted in the app tree is never read.
set -eu

HULL=${HULL:-build/hull}
PY=${PYTHON:-python3}
FIX=tests/fixtures
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }
has() { grep -qF -- "$2" "$1"; }
# `hull test` needs the HTTP server; a pure-compute build has none.
have_test=0
"$HULL" help 2>&1 | grep -qE '^ +test ' && have_test=1

# gen OUT NAME=FILE... - a generation bundle (shared/generation.h).
gen() {
    "$PY" - "$@" <<'PYEOF'
import hashlib, json, sys
out, pairs = sys.argv[1], sys.argv[2:]
entries, payloads = [], b""
for p in pairs:
    name, path = p.split("=", 1)
    data = open(path, "rb").read()
    entries.append({"vfs_name": name,
                    "content_hash": hashlib.sha256(data).hexdigest(),
                    "size": len(data), "origin": {"source": name},
                    "position_map": "identity"})
    payloads += data
m = json.dumps({"format": "hull-pipeline-generation", "version": 1,
                "generation_id": "e2e", "pipeline_plan_hash": "0" * 64,
                "entries": entries, "inputs": []}).encode()
open(out, "wb").write(b"hull-generation 1 %d\n" % len(m) + m + payloads)
PYEOF
}

# run OUT GEN ARGS... - hull with GEN handed over on fd 9.
run() {
    out=$1; g=$2; shift 2
    HULL_GENERATION_FD=9 "$HULL" "$@" > "$out" 2>&1 9< "$g" || echo "exit: $?" >> "$out"
}

for rt in lua js; do
    app=$FIX/generation_$rt
    ext=$rt
    echo "== $rt: the generation shadows what it names"
    if [ "$rt" = lua ]; then
        cat > "$WORK/entry.$ext" <<'LEOF'
app.manifest({ modules = { "hull/template@1" } })
local template = require("hull.template")
local m = require("./lib/m")
local g = require("./lib/gen")
app.main(function(ctx)
    ctx.stdout:write("entry=overlay m=" .. m.v .. " gen=" .. g.v .. " page=" ..
        template.render("page.html", {}) .. "\n")
    return 0
end)
LEOF
        echo 'return { v = "overlay" }' > "$WORK/m.$ext"
        echo 'return { v = "generated" }' > "$WORK/gen.$ext"
        entry=app.lua
    else
        cat > "$WORK/entry.$ext" <<'JEOF'
import { app } from "hull:app";
import { template } from "hull:template";
import { v } from "./lib/m.js";
import { v as g } from "./lib/gen.js";
app.manifest({ modules: ["hull/template@1"] });
app.main((ctx) => {
    ctx.stdout.write("entry=overlay m=" + v + " gen=" + g + " page=" +
        template.render("page.html", {}) + "\n");
    return 0;
});
JEOF
        echo 'export const v = "overlay";' > "$WORK/m.$ext"
        echo 'export const v = "generated";' > "$WORK/gen.$ext"
        entry=app.js
    fi
    printf 'overlay-page' > "$WORK/page.html"
    gen "$WORK/full.gen" "$entry=$WORK/entry.$ext" "lib/m.$ext=$WORK/m.$ext" \
        "lib/gen.$ext=$WORK/gen.$ext" "templates/page.html=$WORK/page.html"

    "$HULL" "$app/$entry" > "$WORK/plain.txt" 2>&1 || true
    if has "$WORK/plain.txt" "entry=disk m=disk page=disk-page"; then
        ok "with no generation the app runs its files"
    else
        bad "no generation"; cat "$WORK/plain.txt"
    fi
    run "$WORK/full.txt" "$WORK/full.gen" "$app/$entry"
    if has "$WORK/full.txt" "entry=overlay m=overlay gen=generated page=overlay-page" &&
       ! has "$WORK/full.txt" "exit:"; then
        ok "entry, module, generated-only module and template come from the generation"
    else
        bad "generation not loaded"; cat "$WORK/full.txt"
    fi

    gen "$WORK/mod.gen" "lib/m.$ext=$WORK/m.$ext"
    run "$WORK/mod.txt" "$WORK/mod.gen" "$app/$entry"
    if has "$WORK/mod.txt" "entry=disk m=overlay page=disk-page"; then
        ok "a generation shadows only the names it holds"
    else
        bad "partial generation"; cat "$WORK/mod.txt"
    fi

    if [ "$have_test" -eq 1 ]; then
        "$HULL" test "$app" > "$WORK/test_plain.txt" 2>&1 && st=0 || st=$?
        run "$WORK/test.txt" "$WORK/mod.gen" test "$app"
        if [ "$st" -ne 0 ] && ! has "$WORK/test.txt" "exit:" &&
           has "$WORK/test.txt" "1/1 tests passed"; then
            ok "hull test loads the generation (and fails without it)"
        else
            bad "hull test"; cat "$WORK/test_plain.txt" "$WORK/test.txt"
        fi
    fi

    echo "== $rt: refusals"
    cp "$WORK/full.gen" "$WORK/bad.gen"
    "$PY" -c 'import sys; p=sys.argv[1]; b=bytearray(open(p,"rb").read()); b[-3]^=1; open(p,"wb").write(b)' "$WORK/bad.gen"
    run "$WORK/bad.txt" "$WORK/bad.gen" "$app/$entry"
    if has "$WORK/bad.txt" "does not match its content_hash" &&
       has "$WORK/bad.txt" "exit:" && ! has "$WORK/bad.txt" "entry="; then
        ok "a payload that fails its hash stops the app; the sources never run"
    else
        bad "corrupt payload"; cat "$WORK/bad.txt"
    fi
    if [ "$have_test" -eq 1 ]; then
        run "$WORK/bad_test.txt" "$WORK/bad.gen" test "$app"
        if has "$WORK/bad_test.txt" "does not match its content_hash" &&
           has "$WORK/bad_test.txt" "exit:" && ! has "$WORK/bad_test.txt" "tests passed"; then
            ok "hull test refuses it too"
        else
            bad "hull test, corrupt payload"; cat "$WORK/bad_test.txt"
        fi
    fi
    gen "$WORK/name.gen" ".hull/x.$ext=$WORK/m.$ext"
    run "$WORK/name.txt" "$WORK/name.gen" "$app/$entry"
    if has "$WORK/name.txt" "bad vfs_name" && ! has "$WORK/name.txt" "entry="; then
        ok "an entry under the reserved .hull is refused"
    else
        bad "reserved name"; cat "$WORK/name.txt"
    fi
    HULL_GENERATION_FD=1 "$HULL" "$app/$entry" > "$WORK/fd.txt" 2>&1 || echo "exit: $?" >> "$WORK/fd.txt"
    if has "$WORK/fd.txt" "names no readable descriptor" && ! has "$WORK/fd.txt" "entry="; then
        ok "a descriptor that is not one (stdout) stops the app"
    else
        bad "bad descriptor"; cat "$WORK/fd.txt"
    fi
done

echo "== nothing on disk is read as a generation"
cp -R "$FIX/generation_lua" "$WORK/planted"
mkdir -p "$WORK/planted/.hull"
gen "$WORK/planted/.hull/generation.json" "lib/m.lua=$WORK/m.lua"
cp "$WORK/planted/.hull/generation.json" "$WORK/planted/generation.json"
"$HULL" "$WORK/planted/app.lua" > "$WORK/planted.txt" 2>&1 || true
if has "$WORK/planted.txt" "entry=disk m=disk page=disk-page"; then
    ok "a generation-shaped file in the tree (and in .hull/) is never read"
else
    bad "planted generation"; cat "$WORK/planted.txt"
fi

[ "$fail" -eq 0 ] && echo "e2e_generation: OK" || { echo "e2e_generation: FAILED"; exit 1; }
