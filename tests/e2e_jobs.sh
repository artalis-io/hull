#!/bin/sh
# e2e_jobs.sh - hull/jobs@1 durable job queue (Lua + JS).
#
# Phase 1: schema + jobs.init (idempotent; full column set).
# Phase 2: enqueue + the atomic claim - correctness (dedup / delay / priority /
#          no double-claim) in both runtimes, plus the CONCURRENCY GATE: N
#          parallel claimer processes against one shared SQLite (WAL) DB must
#          claim each job exactly once. Postgres/MySQL run the same claim SQL
#          (SKIP LOCKED) and are exercised in e2e_postgres / e2e_mysql.
#
# Design: docs/jobs_design.md.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
case "$HULL" in
    /*) ;;
    *)  HULL="$(pwd)/$HULL" ;;
esac

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ - $2}"; }

EXPECT_COLS="id,queue,type,payload,status,priority,attempts,max_attempts,run_at,claim_token,claimed_at,dedup_key,last_error,created_at,updated_at"

# ── Phase 1: init + schema; Phase 2: correctness round-trip (both runtimes) ──
check_runtime() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"; DB="$T/app.db"
    printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$DB" 2>&1)" || true

    case "$out" in
        *JOBS-INIT-OK*) pass "$label: jobs.init() idempotent" ;;
        *) fail "$label: jobs.init()" "$out" ;;
    esac
    case "$out" in *"CORRECT: dedup=1 total=27 dups=0 first=hot"*)
        pass "$label: enqueue/claim - dedup, delay-excluded, no double-claim, priority-first" ;;
        *) fail "$label: enqueue/claim round-trip" "$out" ;;
    esac

    cols="$("$HULL" agent db schema "$T" -d "$DB" 2>/dev/null \
        | python3 -c "import sys,json
d=json.load(sys.stdin)
t=[x for x in d.get('tables',[]) if x.get('name')=='_hull_jobs']
print(','.join(c.get('name') for c in t[0].get('columns',[])) if t else 'MISSING')" 2>/dev/null)"
    [ "$cols" = "$EXPECT_COLS" ] && pass "$label: _hull_jobs full column set" \
        || fail "$label: column set" "got: $cols"
    rm -rf "$T"
}

LUA_APP='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init(); jobs.init()
  ctx.stdout:write("JOBS-INIT-OK\n")
  for i=1,25 do jobs.enqueue("t",{n=i}) end
  local a=jobs.enqueue("t",{},{dedup_key="k1"}); local b=jobs.enqueue("t",{},{dedup_key="k1"})
  jobs.enqueue("later",{},{delay=3600}); jobs.enqueue("hot",{},{priority=100})
  local seen,dup,order,total={},0,{},0
  while true do local batch=jobs.claim({batch=10}); if #batch==0 then break end
    for _,j in ipairs(batch) do total=total+1; if seen[j.id] then dup=dup+1 end; seen[j.id]=true; order[#order+1]=j.type end end
  local ok = (a~=nil and b==nil) and 1 or 0
  ctx.stdout:write(("CORRECT: dedup=%d total=%d dups=%d first=%s\n"):format(ok,total,dup,tostring(order[1])))
  return 0
end)'

JS_APP='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main((ctx) => {
  jobs.init(); jobs.init();
  ctx.stdout.write("JOBS-INIT-OK\n");
  for (let i=0;i<25;i++) jobs.enqueue("t",{n:i});
  const a=jobs.enqueue("t",{},{dedupKey:"k1"}); const b=jobs.enqueue("t",{},{dedupKey:"k1"});
  jobs.enqueue("later",{},{delay:3600}); jobs.enqueue("hot",{},{priority:100});
  const seen={}; let dup=0,total=0; const order=[];
  for(;;){ const batch=jobs.claim({batch:10}); if(batch.length===0) break;
    for(const j of batch){ total++; if(seen[j.id]) dup++; seen[j.id]=true; order.push(j.type); } }
  const ok = (a!==null && b===null) ? 1 : 0;
  ctx.stdout.write(`CORRECT: dedup=${ok} total=${total} dups=${dup} first=${order[0]}\n`);
  return 0;
});'

echo "== Lua =="
check_runtime "lua" "lua" "$LUA_APP"
echo "== JS =="
check_runtime "js" "js" "$JS_APP"

# ── Phase 2: SQLite concurrency gate ────────────────────────────────────────
# N jobs, K parallel claimer PROCESSES against one WAL DB. Each job must be
# claimed exactly once across all claimers (the atomic-claim correctness gate).
CONC=4; N=200
echo "== concurrency (SQLite, $CONC parallel claimers) =="
W="$(mktemp -d)"; DB="$W/jobs.db"

cat > "$W/seed.lua" <<LUA
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function()
  jobs.init()
  for i = 1, $N do jobs.enqueue("t", { n = i }) end
  return 0
end)
LUA
cat > "$W/claim.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  while true do
    local batch = jobs.claim({ batch = 7 })   -- small batch => heavy interleaving
    if #batch == 0 then break end
    for _, j in ipairs(batch) do ctx.stdout:write(j.id .. "\n") end
  end
  return 0
end)
LUA

"$HULL" "$W/seed.lua" -d "$DB" >/dev/null 2>&1
i=1
while [ "$i" -le "$CONC" ]; do
    "$HULL" "$W/claim.lua" -d "$DB" > "$W/out.$i" 2>/dev/null &
    i=$((i + 1))
done
wait

total="$(cat "$W"/out.* | grep -c . || true)"
uniq="$(cat "$W"/out.* | sort -n | uniq | grep -c . || true)"
if [ "$total" -eq "$N" ] && [ "$uniq" -eq "$N" ]; then
    pass "concurrency: $N jobs claimed exactly once by $CONC parallel processes"
else
    fail "concurrency: expected $N claimed once" "total=$total uniq=$uniq"
fi
rm -rf "$W"

echo ""
echo "=== Summary ==="
echo "PASSED: $PASS"
echo "FAILED: $FAIL"
[ "$FAIL" -eq 0 ]
