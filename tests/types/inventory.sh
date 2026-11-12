#!/bin/sh
# Writes the live JavaScript export inventory of every registry module, one
# JSON object per line, to stdout. Usage: inventory.sh <hull binary>
#
# Each module is loaded by a throwaway app that declares it and walks its
# exports (own properties, 4 levels). The result feeds drift.mjs, which turns
# it into type-level assertions tsc checks against types/hull.
set -eu
HULL=$1
case $HULL in /*) ;; *) HULL=$(pwd)/$HULL ;; esac
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

walker='
function desc(v, depth, seen) {
  const t = typeof v;
  if (t === "function") {
    const o = { kind: "function" };
    const ks = Object.getOwnPropertyNames(v).filter(k => !["length", "name", "prototype", "arguments", "caller"].includes(k));
    if (ks.length && depth > 0) { o.props = {}; for (const k of ks) o.props[k] = desc(v[k], depth - 1, seen); }
    return o;
  }
  if (v === null) return { kind: "null" };
  if (t === "object") {
    if (seen.includes(v)) return { kind: "cycle" };
    if (Array.isArray(v)) return { kind: "array" };
    const o = { kind: "object" };
    if (depth > 0) {
      o.props = {};
      for (const k of Object.getOwnPropertyNames(v)) {
        let x;
        try { x = v[k]; } catch (e) { o.props[k] = { kind: "throws" }; continue; }
        o.props[k] = desc(x, depth - 1, seen.concat([v]));
      }
    }
    return o;
  }
  return { kind: t };
}'

run_app() {  # $1 = JS module name, $2 = manifest extra, $3 = modules list, $4 = import line
    d=$WORK/app; rm -rf "$d"; mkdir -p "$d"
    cat > "$d/app.js" <<JS
$4
import { app } from "hull:app";
app.manifest({ $2 modules: [$3] });
$walker
app.main((ctx) => {
  const out = {};
  for (const k of Object.keys(M)) out[k] = desc(M[k], 4, []);
  ctx.stdout.write(JSON.stringify({ module: "$1", exports: out }) + "\n");
  return 0;
});
JS
    if (cd "$d" && "$HULL" app.js > out.json 2> err.txt); then
        cat "$d/out.json"
    elif grep -q 'requires HL_ENABLE_' "$d/err.txt"; then
        printf '{"module":"%s","unavailable":true}\n' "$1"
    elif grep -q 'unknown hull module' "$d/err.txt"; then
        printf '{"module":"%s","not_js":true}\n' "$1"
    else
        echo "inventory: $1 failed to load:" >&2; cat "$d/err.txt" >&2; exit 1
    fi
}

# The app intrinsic, with the modules that decorate it declared.
run_app "hull:app" "" '"hull/http-server@1", "hull/timers@1", "hull/web/ws-server@1", "hull/web/sse@1"' 'import { app as __app } from "hull:app"; const M = { app: __app };'

"$HULL" modules available --json > "$WORK/mods.json"
for spec in $(tr "," "\n" < "$WORK/mods.json" | sed -n 's/.*"name": *"\(hull\/[^"]*\)".*/\1/p'); do
    [ "$spec" = hull/app ] && continue
    name="hull:$(echo "${spec#hull/}" | tr / :)"
    extra=""; [ "$spec" = hull/tui ] && extra="tui: true,"
    run_app "$name" "$extra" "\"$spec@1\"" "import * as M from \"$name\";"
done
