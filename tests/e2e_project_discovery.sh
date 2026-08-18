#!/bin/sh
# E2E tests — `hull agent inspect` (standalone project source discovery)
#
# Usage: sh tests/e2e_project_discovery.sh
# Requires: build/hull already built; python3 (for JSON assertions, as the oauth e2e uses).
#
# Exercises the REAL tool VM end-to-end (not the unit harness's stubbed fs): the canonical
# analyzer produces deterministic output; Lua annotations are discovered WITHOUT executing
# app source; unknown annotations survive; malformed Lua -> valid=false + diagnostics;
# frontend capabilities are accurate; static/ browser assets are pruned while application
# .js is honestly unsupported (never parsed as Lua) -> complete=false; a missing root is
# an invalid+incomplete discovery; and the public JSON leaks NO generation-internal state
# (handle / _by_source / _handles / by_id). Slice 3 also drives a live `hull dev --agent`:
# it publishes a discovery.json generation per reload (session_pid-bound), inspect serves
# the LIVE generation, a source change bumps the generation, and a dead/stale session
# sidecar is ignored (falls back to standalone).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0

if [ ! -x "$HULL" ]; then
    echo "e2e-project-discovery: hull binary not found at $HULL — run 'make' first"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "e2e-project-discovery: python3 not found — skipping (JSON assertions need it)"
    exit 0
fi

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

DEV_PID=""
TMP=$(mktemp -d)
trap '[ -n "$DEV_PID" ] && kill "$DEV_PID" 2>/dev/null; chmod -R u+rwx "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT

# assert_py "<name>" "<json>" "<python expr over d>": pass iff the expr is truthy.
assert_py() {
    _name="$1"; _json="$2"; _expr="$3"
    if printf '%s' "$_json" | python3 -c '
import json,sys
d=json.load(sys.stdin)
sys.exit(0 if ('"$_expr"') else 1)
' 2>/dev/null; then pass "$_name"; else fail "$_name"; fi
}

echo ""
echo "=== E2E: hull agent inspect (project source discovery) ==="

# ── a mixed modular app: annotated Lua, nested source, browser asset, app JS ──
APP="$TMP/app"
mkdir -p "$APP/routes" "$APP/static"
cat > "$APP/app.lua" <<'EOF'
---@route GET /
---@custom freeform tag
local function home() end
local a, b = 1, 2
return home, a, b
EOF
cat > "$APP/routes/users.lua" <<'EOF'
---@resource users
local function list() end
return list
EOF
printf 'window.x = 1;\n' > "$APP/static/vendor.js"   # browser asset -> pruned
printf 'export const q = 1;\n' > "$APP/client.js"    # application JS -> unsupported

OUT=$("$HULL" agent inspect "$APP" 2>/dev/null); RC=$?
[ "$RC" = "0" ] && pass "exit 0 (a discovery was produced)" || fail "exit code ($RC)"

assert_py "schema_version + standalone provenance" "$OUT" 'd["schema_version"]==1 and d["source"]=="standalone" and d["generation"]==0'
assert_py "valid (no malformed source)" "$OUT" 'd["valid"] is True'
assert_py "complete=false (an application .js is unsupported)" "$OUT" 'd["complete"] is False'
assert_py "static/*.js browser asset is pruned (not a source)" "$OUT" \
    'not any(s["path"]=="static/vendor.js" for s in d["sources"])'
assert_py "application client.js honestly unsupported" "$OUT" \
    'any(s["path"]=="client.js" and s["status"]=="unsupported" and s["language"]=="javascript" for s in d["sources"])'
assert_py "app.lua + routes/users.lua analyzed" "$OUT" \
    'sum(1 for s in d["sources"] if s["status"]=="analyzed")==2'
assert_py "annotations discovered WITHOUT executing source" "$OUT" \
    'set(d["indexes"]["by_annotation"].keys())=={"route","custom","resource"}'
assert_py "UNKNOWN @custom annotation survives" "$OUT" \
    'any(a["name"]=="custom" for x in d["declarations"] for a in x["annotations"])'
assert_py "annotated-only public declarations (home + list)" "$OUT" \
    'sorted(x["name"] for x in d["declarations"])==["home","list"]'
assert_py "totals retain all 4 declarations; 2 annotated" "$OUT" \
    'd["summary"]["declarations_total"]==4 and d["summary"]["declarations_annotated"]==2'
assert_py "JS frontend honest: analyzable=false, 0 capabilities" "$OUT" \
    'any(f["language"]=="javascript" and f["analyzable"] is False and len(f["capabilities"])==0 for f in d["frontends"])'
assert_py "Lua frontend reports 4 shipped capabilities" "$OUT" \
    'any(f["language"]=="lua" and f["analyzable"] is True and len(f["capabilities"])==4 for f in d["frontends"])'
assert_py "unsupported-frontend diagnostic present (warning)" "$OUT" \
    'any(x["code"]=="project.frontend.unsupported" and x["severity"]=="warning" for x in d["diagnostics"])'

# multi-name group: both @route/@custom annotations target the SAME declaration group
assert_py "annotation target_group_id points at the declaration group" "$OUT" \
    'all(a["target_group_id"]==x["group_id"] for x in d["declarations"] for a in x["annotations"])'

# NO generation-internal state on the wire
assert_py "no handle / _by_source / _handles / by_id leaked" "$OUT" \
    '"handle" not in json.dumps(d) and "_by_source" not in json.dumps(d) and "_handles" not in json.dumps(d) and "by_id" not in d["indexes"]'

# determinism
OUT2=$("$HULL" agent inspect "$APP" 2>/dev/null)
[ "$OUT" = "$OUT2" ] && pass "deterministic (two runs byte-identical)" || fail "non-deterministic output"

# ── malformed Lua -> valid=false, structured diagnostics, status error ──
BAD="$TMP/bad"
mkdir -p "$BAD"
printf 'local function ok() end\nreturn ok\n' > "$BAD/good.lua"
printf 'local = = )\n' > "$BAD/broken.lua"
OUTB=$("$HULL" agent inspect "$BAD" 2>/dev/null)
assert_py "malformed Lua -> valid=false" "$OUTB" 'd["valid"] is False'
assert_py "malformed source status=error + error diagnostic" "$OUTB" \
    'any(s["path"]=="broken.lua" and s["status"]=="error" for s in d["sources"]) and any(x["severity"]=="error" for x in d["diagnostics"])'
assert_py "the clean sibling still analyzed" "$OUTB" \
    'any(s["path"]=="good.lua" and s["status"]=="analyzed" for s in d["sources"])'

# ── a genuinely clean all-Lua project -> valid + complete ──
OK="$TMP/okproj"
mkdir -p "$OK/lib"
cat > "$OK/app.lua" <<'EOF'
---@main
local function main() end
return main
EOF
cat > "$OK/lib/util.lua" <<'EOF'
---@util
local function u() end
return u
EOF
OUTC=$("$HULL" agent inspect "$OK" 2>/dev/null)
assert_py "clean all-Lua project -> valid AND complete" "$OUTC" 'd["valid"] is True and d["complete"] is True'
assert_py "both annotated Lua declarations public" "$OUTC" 'sorted(x["name"] for x in d["declarations"])==["main","u"]'

# ── missing root -> invalid + incomplete, project.discovery_failed, still exit 0 ──
OUTM=$("$HULL" agent inspect "$TMP/does-not-exist" 2>/dev/null); RCM=$?
assert_py "missing root -> invalid + incomplete + no sources" "$OUTM" \
    'd["valid"] is False and d["complete"] is False and len(d["sources"])==0'
assert_py "missing root emits project.discovery_failed" "$OUTM" \
    'any(x["code"]=="project.discovery_failed" for x in d["diagnostics"])'
[ "$RCM" = "0" ] && pass "missing root still exits 0 (validity is in the JSON)" || fail "missing-root exit ($RCM)"

# ── usage: multiple positional roots is an error (exit 2), not "use the last" ──
# (|| RC=$? captures the non-zero exit without tripping `set -e`.)
RCU=0; "$HULL" agent inspect "$OK" "$BAD" >/dev/null 2>&1 || RCU=$?
[ "$RCU" = "2" ] && pass "two positional roots -> usage error (exit 2)" || fail "multi-positional exit ($RCU, want 2)"
# a single positional still works (exit 0)
RCS=0; "$HULL" agent inspect "$OK" >/dev/null 2>&1 || RCS=$?
[ "$RCS" = "0" ] && pass "single positional root still exits 0" || fail "single-positional exit ($RCS)"
# an unknown flag is a usage error too
RCF=0; "$HULL" agent inspect --bogus >/dev/null 2>&1 || RCF=$?
[ "$RCF" = "2" ] && pass "unknown flag -> usage error (exit 2)" || fail "unknown-flag exit ($RCF, want 2)"

# ── internal publish flags require BOTH --generation and --session-pid (all-or-none) ──
RCP=0; "$HULL" agent inspect "$OK" --generation=5 >/dev/null 2>&1 || RCP=$?
[ "$RCP" = "2" ] && pass "publish: --generation without --session-pid -> exit 2" || fail "partial publish exit ($RCP, want 2)"
RCP=0; "$HULL" agent inspect "$OK" --session-pid=5 >/dev/null 2>&1 || RCP=$?
[ "$RCP" = "2" ] && pass "publish: --session-pid without --generation -> exit 2" || fail "partial publish exit ($RCP, want 2)"
# a full publish writes the CANONICAL <app_dir>/.hull/discovery.json (no caller path), tagged dev
rm -f "$OK/.hull/discovery.json" 2>/dev/null || true
"$HULL" agent inspect "$OK" --generation=3 --session-pid="$$" >/dev/null 2>&1 || true
if [ -f "$OK/.hull/discovery.json" ]; then
    pass "publish writes the canonical .hull/discovery.json"
    assert_py "published file tagged source=dev, generation=3, session_pid" "$(cat "$OK/.hull/discovery.json")" \
        "d['source']=='dev' and d['generation']==3 and d.get('session_pid')==$$"
else fail "publish did not write the canonical discovery.json"; fi
rm -rf "$OK/.hull" 2>/dev/null || true

# ── Slice 3: hull dev --agent publishes generations; inspect reads the LIVE one ──
DEVAPP="$TMP/devapp"
mkdir -p "$DEVAPP"
cat > "$DEVAPP/app.lua" <<'EOF'
---@route GET /
local function home() end
return home
EOF
"$HULL" dev --agent -p 39811 "$DEVAPP/app.lua" >/dev/null 2>&1 &
DEV_PID=$!
# wait for the first published generation (up to ~10s)
i=0; while [ "$i" -lt 20 ] && [ ! -f "$DEVAPP/.hull/discovery.json" ]; do sleep 0.5; i=$((i + 1)); done

if [ -f "$DEVAPP/.hull/discovery.json" ]; then
    OUTD=$("$HULL" agent inspect "$DEVAPP" 2>/dev/null)
    assert_py "dev running -> published generation served (source=dev, generation>=1)" "$OUTD" \
        'd["source"]=="dev" and d["generation"]>=1'
    assert_py "published generation carries a session_pid" "$OUTD" 'd.get("session_pid") is not None'
    SP_DISC=$(grep -o '"session_pid":[0-9]*' "$DEVAPP/.hull/discovery.json" | head -1)
    SP_DEV=$(grep -o '"session_pid":[0-9]*' "$DEVAPP/.hull/dev.json" | head -1)
    { [ -n "$SP_DISC" ] && [ "$SP_DISC" = "$SP_DEV" ]; } \
        && pass "discovery.json + dev.json session_pid match" || fail "session_pid mismatch ($SP_DISC vs $SP_DEV)"

    # invalid public CLI forms MUST NOT bypass validation via the live fast path
    RCX=0; "$HULL" agent inspect "$DEVAPP" extra >/dev/null 2>&1 || RCX=$?
    [ "$RCX" = "2" ] && pass "live: extra positional -> exit 2 (not streamed)" || fail "live extra-positional exit ($RCX, want 2)"
    RCX=0; "$HULL" agent inspect "$DEVAPP" --bogus >/dev/null 2>&1 || RCX=$?
    [ "$RCX" = "2" ] && pass "live: unknown flag -> exit 2 (not streamed)" || fail "live unknown-flag exit ($RCX, want 2)"
    HOUT=$("$HULL" agent inspect "$DEVAPP" --help 2>/dev/null || true)
    { echo "$HOUT" | grep -q "usage:" && ! echo "$HOUT" | grep -q '"source"'; } \
        && pass "live: --help prints usage (not streamed)" || fail "live --help did not print usage"

    GEN1=$(printf '%s' "$OUTD" | python3 -c 'import json,sys;print(json.load(sys.stdin)["generation"])' 2>/dev/null || echo 0)
    # a source change triggers a reload -> a NEW generation
    printf '\n---@added\nlocal x = 1\nreturn home, x\n' >> "$DEVAPP/app.lua"
    NEWGEN=0; i=0
    while [ "$i" -lt 30 ]; do
        G=$("$HULL" agent inspect "$DEVAPP" 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin).get("generation",0))' 2>/dev/null || echo 0)
        if [ "$G" -gt "$GEN1" ] 2>/dev/null; then NEWGEN=$G; break; fi
        sleep 0.5; i=$((i + 1))
    done
    { [ "$NEWGEN" -gt "$GEN1" ]; } 2>/dev/null \
        && pass "source change -> reload -> new generation ($GEN1 -> $NEWGEN)" \
        || fail "no new generation after reload (gen1=$GEN1, newgen=$NEWGEN)"

    # publisher FAILURE: break the atomic write target (a dir where the .tmp file goes) so
    # the next reload's publish fails. The prior same-session generation must be REMOVED
    # (dev.c) so inspect falls back to standalone rather than serving a stale generation.
    mkdir "$DEVAPP/.hull/discovery.json.tmp"
    printf '\n-- force another reload\n' >> "$DEVAPP/app.lua"
    STANDALONE=0; i=0
    while [ "$i" -lt 30 ]; do
        S=$("$HULL" agent inspect "$DEVAPP" 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin)["source"])' 2>/dev/null || echo "")
        if [ "$S" = "standalone" ]; then STANDALONE=1; break; fi
        sleep 0.5; i=$((i + 1))
    done
    [ "$STANDALONE" = "1" ] \
        && pass "publisher failure -> stale generation removed -> standalone" \
        || fail "stale generation still served after publisher failure"
    rmdir "$DEVAPP/.hull/discovery.json.tmp" 2>/dev/null || true

    kill "$DEV_PID" 2>/dev/null || true; wait "$DEV_PID" 2>/dev/null || true; DEV_PID=""
    { [ ! -f "$DEVAPP/.hull/discovery.json" ] && [ ! -f "$DEVAPP/.hull/dev.json" ]; } \
        && pass "sidecars removed on dev exit" || fail "sidecars linger after dev exit"
    OUTS=$("$HULL" agent inspect "$DEVAPP" 2>/dev/null)
    assert_py "after dev stops -> standalone (generation 0)" "$OUTS" \
        'd["source"]=="standalone" and d["generation"]==0'
else
    fail "dev did not publish discovery.json within timeout"
    kill "$DEV_PID" 2>/dev/null || true; wait "$DEV_PID" 2>/dev/null || true; DEV_PID=""
fi

# ── stale/crashed-session robustness: sidecars with a DEAD session_pid -> standalone ──
STALE="$TMP/stale"
mkdir -p "$STALE/.hull"
cat > "$STALE/app.lua" <<'EOF'
---@main
local function m() end
return m
EOF
DEADPID=2147480000   # far above any real pid -> kill(pid,0) fails -> not live
printf '{"port":1,"pid":1,"session_pid":%s,"started_at":1}\n' "$DEADPID" > "$STALE/.hull/dev.json"
printf '{"schema_version":1,"source":"dev","generation":9,"session_pid":%s,"declarations":[]}\n' "$DEADPID" > "$STALE/.hull/discovery.json"
OUTST=$("$HULL" agent inspect "$STALE" 2>/dev/null)
assert_py "dead-session sidecar ignored -> fresh standalone analysis" "$OUTST" \
    'd["source"]=="standalone" and any(x["name"]=="m" for x in d["declarations"])'

# ── malformed / truncated sidecars -> standalone (a LIVE session_pid so ONLY the
#    malformation, not liveness, drives the fallback; $$ = this shell, alive) ──
MAL="$TMP/malformed"
mkdir -p "$MAL/.hull"
cat > "$MAL/app.lua" <<'EOF'
---@main
local function m() end
return m
EOF
# (a) discovery.json session_pid is a non-integer token
printf '{"port":1,"pid":1,"session_pid":%s,"started_at":1}\n' "$$" > "$MAL/.hull/dev.json"
printf '{"schema_version":1,"source":"dev","generation":9,"session_pid":"12x3","declarations":[]}\n' > "$MAL/.hull/discovery.json"
OUTM1=$("$HULL" agent inspect "$MAL" 2>/dev/null)
assert_py "malformed discovery session_pid -> standalone" "$OUTM1" 'd["source"]=="standalone"'
# (b) dev.json session_pid malformed
printf '{"port":1,"pid":1,"session_pid":"nan","started_at":1}\n' > "$MAL/.hull/dev.json"
printf '{"schema_version":1,"source":"dev","generation":9,"session_pid":%s,"declarations":[]}\n' "$$" > "$MAL/.hull/discovery.json"
OUTM2=$("$HULL" agent inspect "$MAL" 2>/dev/null)
assert_py "malformed dev session_pid -> standalone" "$OUTM2" 'd["source"]=="standalone"'
# (c) discovery.json truncated BEFORE the session_pid field
printf '{"port":1,"pid":1,"session_pid":%s,"started_at":1}\n' "$$" > "$MAL/.hull/dev.json"
printf '{"schema_version":1,"source":"dev","generat' > "$MAL/.hull/discovery.json"
OUTM3=$("$HULL" agent inspect "$MAL" 2>/dev/null)
assert_py "truncated (before PID) discovery sidecar -> standalone" "$OUTM3" 'd["source"]=="standalone"'
# (d) discovery.json truncated IMMEDIATELY AFTER a valid matching session_pid (no closing
#     brace) -- the session_pid token alone would match, but the DOCUMENT is incomplete
printf '{"session_pid":%s' "$$" > "$MAL/.hull/discovery.json"
OUTM4=$("$HULL" agent inspect "$MAL" 2>/dev/null)
assert_py "truncated right after a matching PID -> standalone (envelope invalid)" "$OUTM4" 'd["source"]=="standalone"'
# (e) discovery.json truncated LATER in the document (valid PID, then cut mid-structure)
printf '{"session_pid":%s,"declarations":[{"id":"x","annota' "$$" > "$MAL/.hull/discovery.json"
OUTM5=$("$HULL" agent inspect "$MAL" 2>/dev/null)
assert_py "truncated later in the document -> standalone (envelope invalid)" "$OUTM5" 'd["source"]=="standalone"'
# sanity: a COMPLETE valid doc with a matching live PID IS served (proves the gate isn't
# rejecting everything)
printf '{"schema_version":1,"source":"dev","generation":7,"session_pid":%s,"declarations":[]}\n' "$$" > "$MAL/.hull/discovery.json"
OUTM6=$("$HULL" agent inspect "$MAL" 2>/dev/null)
assert_py "complete valid doc + live matching PID IS served (source=dev)" "$OUTM6" 'd["source"]=="dev" and d["generation"]==7'
# (f) valid JSON with a matching PID, then an embedded NUL + trailing garbage: the COMPLETE
#     on-disk document is invalid -> standalone (must parse/emit the exact byte length, not
#     stop at the NUL via strlen/fputs)
{ printf '{"schema_version":1,"source":"dev","generation":8,"session_pid":%s,"declarations":[]}' "$$"; printf '\000trailing-garbage'; } > "$MAL/.hull/discovery.json"
OUTM7=$("$HULL" agent inspect "$MAL" 2>/dev/null)
assert_py "embedded NUL + trailing garbage -> standalone (byte-length validation)" "$OUTM7" \
    'd["source"]=="standalone"'

# ── build-readiness (Slice 4): the analyzer runs on a REAL repository example tree ──
if [ -d examples/hello ]; then
    OUTE=$("$HULL" agent inspect examples/hello 2>/dev/null)
    assert_py "analyzes a real example app tree (valid, >=1 Lua source analyzed)" "$OUTE" \
        'd["valid"] is True and d["summary"]["sources_analyzed"] >= 1'
fi

echo ""
echo "=== hull agent inspect E2E: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
