#!/bin/sh
# e2e_jobs.sh - hull/jobs@1 durable job queue (Lua + JS).
#
# Phase 1: schema + jobs.init (idempotent; full column set).
# Phase 2: enqueue + the atomic claim - correctness (dedup / delay / priority /
#          no double-claim) in both runtimes, plus the CONCURRENCY GATE: N
#          parallel claimer processes against one shared SQLite (WAL) DB must
#          claim each job exactly once. Postgres/MySQL run the same claim SQL
#          (SKIP LOCKED) and are exercised in e2e_postgres / e2e_mysql.
# Phase 3: handlers + work loop + outcomes (done / dead-letter / retry-backoff),
#          the optional catch-all default handler, and the visibility-timeout
#          reaper - in both runtimes.
# Phase 4: the dedicated worker - jobs.run_worker (drain + jobs.stop graceful
#          exit) in both runtimes, plus the worker-mode concurrency gate driven
#          through the `hull jobs worker` CLI (K workers, exactly-once).
# Phase 5: the ops surface - jobs.dead (list) / jobs.retry (requeue) /
#          jobs.cleanup (purge terminal rows) in both runtimes.
#
# Design: docs/jobs_design.md. User guide: docs/jobs.md.
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

# ── Phase 3: handlers, work-loop outcomes, catch-all, reaper (both runtimes) ─
# One app exercises every outcome path and prints a single parseable line:
#   P3 done=<n> dead=<n> caught=<n> retry_pending=<n> reclaimed=<n>
# Expected: done=1 (the "ok" job), dead=3 (fail-exhausted + DEAD + no-handler),
# caught=1 (a mystery type routed to the default handler in a 2nd DB), a failing
# job with attempts left goes back to pending (retry_pending=1), and a claimed
# job is reclaimed by the vt=0 reaper (reclaimed=1).
check_phase3() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"
    printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"P3 done=1 dead=3 caught=1 retry_pending=1 reclaimed=1"*)
            pass "$label: work-loop outcomes (done/dead/retry) + catch-all + reaper" ;;
        *) fail "$label: phase-3 work loop" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_P3='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  jobs.handler("ok",    function(j) return end)
  jobs.handler("fail",  function(j) error("boom") end)
  jobs.handler("bad",   function(j) return jobs.DEAD end)
  jobs.handler("retry", function(j) error("transient") end)
  jobs.enqueue("ok",{}); jobs.enqueue("fail",{},{max_attempts=1})
  jobs.enqueue("bad",{}); jobs.enqueue("nohandler",{})
  jobs.enqueue("retry",{},{max_attempts=5})
  jobs.work({ batch = 10 })
  local s = jobs.stats()
  -- catch-all: the retry job is pending with a FUTURE run_at, so the next
  -- work() only claims "mystery" and routes it through jobs.default.
  jobs.enqueue("mystery",{})
  jobs.default(function(j) return end)
  jobs.work({ batch = 10 })
  local s2 = jobs.stats()
  local caught = s2.done - s.done   -- the mystery job
  -- reaper: claim a fresh job, reclaim it immediately with vt=0.
  jobs.enqueue("ok",{})
  jobs.claim({ batch = 1 })
  local reclaimed = jobs.reap({ visibility_timeout = 0 })
  ctx.stdout:write(("P3 done=%d dead=%d caught=%d retry_pending=%d reclaimed=%d\n"):format(
    s.done, s.dead, caught, s.pending, reclaimed))
  return 0
end)'

JS_P3='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  jobs.handler("ok",    (j) => {});
  jobs.handler("fail",  (j) => { throw new Error("boom"); });
  jobs.handler("bad",   (j) => jobs.DEAD);
  jobs.handler("retry", (j) => { throw new Error("transient"); });
  jobs.enqueue("ok",{}); jobs.enqueue("fail",{},{maxAttempts:1});
  jobs.enqueue("bad",{}); jobs.enqueue("nohandler",{});
  jobs.enqueue("retry",{},{maxAttempts:5});
  await jobs.work({ batch: 10 });
  const s = jobs.stats();
  jobs.enqueue("mystery",{});
  jobs.default((j) => {});
  await jobs.work({ batch: 10 });
  const s2 = jobs.stats();
  const caught = s2.done - s.done;
  jobs.enqueue("ok",{});
  jobs.claim({ batch: 1 });
  const reclaimed = await jobs.reap({ visibilityTimeout: 0 });
  ctx.stdout.write(`P3 done=${s.done} dead=${s.dead} caught=${caught} retry_pending=${s.pending} reclaimed=${reclaimed}\n`);
  return 0;
});'

echo "== Phase 3: Lua =="
check_phase3 "lua" "lua" "$LUA_P3"
echo "== Phase 3: JS =="
check_phase3 "js" "js" "$JS_P3"

# ── Phase 4: dedicated worker - run_worker drain + stop, both runtimes ───────
# The worker app enqueues 5, drains via run_worker({drain=true}), then proves
# jobs.stop() exits a non-drain loop mid-batch. Emits: P4 processed=5 stopped=3
check_phase4() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"
    printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"P4 processed=5 stopped=3"*)
            pass "$label: run_worker drain + jobs.stop graceful exit" ;;
        *) fail "$label: phase-4 worker loop" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_P4='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  jobs.handler("d", function(j) return end)
  for i = 1, 5 do jobs.enqueue("d", {}) end
  local processed = jobs.run_worker({ drain = true, poll_ms = 10 })
  -- stop(): 3 more jobs, handler stops the (non-drain) loop on the first; the
  -- batch still finishes, then the loop exits at the next _running check.
  local done = 0
  jobs.handler("s", function(j) done = done + 1; if done == 1 then jobs.stop() end end)
  for i = 1, 3 do jobs.enqueue("s", {}) end
  jobs.run_worker({ poll_ms = 10 })
  ctx.stdout:write(("P4 processed=%d stopped=%d\n"):format(processed, done))
  return 0
end)'

JS_P4='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  jobs.handler("d", (j) => {});
  for (let i = 1; i <= 5; i++) jobs.enqueue("d", {});
  const processed = await jobs.runWorker({ drain: true, pollMs: 10 });
  let done = 0;
  jobs.handler("s", (j) => { done++; if (done === 1) jobs.stop(); });
  for (let i = 1; i <= 3; i++) jobs.enqueue("s", {});
  await jobs.runWorker({ pollMs: 10 });
  ctx.stdout.write(`P4 processed=${processed} stopped=${done}\n`);
  return 0;
});'

echo "== Phase 4: Lua =="
check_phase4 "lua" "lua" "$LUA_P4"
echo "== Phase 4: JS =="
check_phase4 "js" "js" "$JS_P4"

# ── Phase 4: worker-mode concurrency via `hull jobs worker` ──────────────────
# The Phase-2 claim gate, but driven through the dedicated-worker CLI: K
# `hull jobs worker` processes drain one shared WAL DB; each job's handler runs
# exactly once across all workers (no double-processing).
echo "== worker concurrency ($CONC `hull jobs worker` processes) =="
W="$(mktemp -d)"; DB="$W/jobs.db"
cat > "$W/seed.lua" <<LUA
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function() jobs.init(); for i=1,$N do jobs.enqueue("w",{n=i}) end; return 0 end)
LUA
cat > "$W/worker.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  jobs.handler("w", function(j) ctx.stdout:write(j.data.n .. "\n") end)
  jobs.run_worker({ drain = true, batch = 7, poll_ms = 5 })
  return 0
end)
LUA

"$HULL" "$W/seed.lua" -d "$DB" >/dev/null 2>&1
i=1
while [ "$i" -le "$CONC" ]; do
    "$HULL" jobs worker "$W/worker.lua" -d "$DB" > "$W/out.$i" 2>/dev/null &
    i=$((i + 1))
done
wait

total="$(cat "$W"/out.* | grep -c . || true)"
uniq="$(cat "$W"/out.* | sort -n | uniq | grep -c . || true)"
if [ "$total" -eq "$N" ] && [ "$uniq" -eq "$N" ]; then
    pass "worker concurrency: $N jobs processed exactly once by $CONC workers"
else
    fail "worker concurrency: expected $N processed once" "total=$total uniq=$uniq"
fi
rm -rf "$W"

# ── Phase 5: ops surface - dead / retry / cancel / cleanup (both runtimes) ───
# Two jobs dead-letter; jobs.dead lists them (with last_error); jobs.retry
# requeues one (and no-ops on a bad id); jobs.cleanup purges the remaining
# terminal row while leaving the requeued pending job; jobs.cancel deletes a
# delayed pending job (and no-ops the second time / on a live job). Emits:
#   P5 dead=2 retry_ok=1 retry_bad=0 purged=1 dead_after=0 pending_after=1
#      has_err=1 cancel_ok=1 cancel_bad=0
check_phase5() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"
    printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"P5 dead=2 retry_ok=1 retry_bad=0 purged=1 dead_after=0 pending_after=1 has_err=1 cancel_ok=1 cancel_bad=0"*)
            pass "$label: ops surface (dead / retry / cancel / cleanup)" ;;
        *) fail "$label: phase-5 ops surface" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_P5='local jobs = require("hull.jobs")
local time = require("hull.time")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  jobs.handler("boom", function(j) error("always fails") end)
  jobs.enqueue("boom",{n=1},{max_attempts=1}); jobs.enqueue("boom",{n=2},{max_attempts=1})
  jobs.work({ batch = 10 })                       -- both dead-letter
  local d = jobs.dead()
  local has_err = (d[1] and d[1].last_error and #d[1].last_error > 0) and 1 or 0
  local ok  = jobs.retry(d[1].id) and 1 or 0      -- requeue one
  local bad = jobs.retry(99999) and 1 or 0        -- non-existent -> false
  local purged = jobs.cleanup({ before = time.now() + 1 })
  local s = jobs.stats()                          -- capture before the cancel test
  -- cancel a delayed (pending) job; the 2nd cancel no-ops (already gone).
  local cid = jobs.enqueue("later",{},{delay=3600})
  local cok  = jobs.cancel(cid) and 1 or 0
  local cbad = jobs.cancel(cid) and 1 or 0
  ctx.stdout:write(("P5 dead=%d retry_ok=%d retry_bad=%d purged=%d dead_after=%d pending_after=%d has_err=%d cancel_ok=%d cancel_bad=%d\n"):format(
    #d, ok, bad, purged, s.dead, s.pending, has_err, cok, cbad))
  return 0
end)'

JS_P5='import { app } from "hull:app"; import { jobs } from "hull:jobs"; import { time } from "hull:time";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  jobs.handler("boom", (j) => { throw new Error("always fails"); });
  jobs.enqueue("boom",{n:1},{maxAttempts:1}); jobs.enqueue("boom",{n:2},{maxAttempts:1});
  await jobs.work({ batch: 10 });
  const d = jobs.dead();
  const hasErr = (d[0] && d[0].lastError && d[0].lastError.length > 0) ? 1 : 0;
  const ok  = jobs.retry(d[0].id) ? 1 : 0;
  const bad = jobs.retry(99999) ? 1 : 0;
  const purged = jobs.cleanup({ before: time.now() + 1 });
  const s = jobs.stats();
  const cid = jobs.enqueue("later",{},{delay:3600});
  const cok  = jobs.cancel(cid) ? 1 : 0;
  const cbad = jobs.cancel(cid) ? 1 : 0;
  ctx.stdout.write(`P5 dead=${d.length} retry_ok=${ok} retry_bad=${bad} purged=${purged} dead_after=${s.dead} pending_after=${s.pending} has_err=${hasErr} cancel_ok=${cok} cancel_bad=${cbad}\n`);
  return 0;
});'

echo "== Phase 5: Lua =="
check_phase5 "lua" "lua" "$LUA_P5"
echo "== Phase 5: JS =="
check_phase5 "js" "js" "$JS_P5"

# ── v1.1: intra-process concurrency (run_worker concurrency=N) ───────────────
# 12 jobs, concurrency=4, batch=1; handlers sleep so claims overlap. All 12 must
# be processed exactly once AND max-in-flight must exceed 1 (real parallelism).
check_conc() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"; printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"CONC ok=1"*) pass "$label: run_worker concurrency (N in-flight, exactly-once)" ;;
        *) fail "$label: v1.1 concurrency" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_CONC='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  local inflight, maxf = 0, 0
  jobs.handler("t", function(j)
    inflight = inflight + 1; if inflight > maxf then maxf = inflight end
    hull.sleep(30); inflight = inflight - 1
  end)
  for i=1,12 do jobs.enqueue("t", {}) end
  local total = jobs.run_worker({ drain = true, concurrency = 4, batch = 1, poll_ms = 5 })
  ctx.stdout:write(("CONC ok=%d total=%d max=%d\n"):format(
    (total==12 and maxf>=2) and 1 or 0, total, maxf))
  return 0
end)'

JS_CONC='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  let inflight = 0, maxf = 0;
  jobs.handler("t", async (j) => {
    inflight++; if (inflight > maxf) maxf = inflight;
    await hull.sleep(30); inflight--;
  });
  for (let i=0;i<12;i++) jobs.enqueue("t", {});
  const total = await jobs.runWorker({ drain: true, concurrency: 4, batch: 1, pollMs: 5 });
  ctx.stdout.write(`CONC ok=${(total===12 && maxf>=2)?1:0} total=${total} max=${maxf}\n`);
  return 0;
});'

echo "== v1.1 concurrency: Lua =="; check_conc "lua" "lua" "$LUA_CONC"
echo "== v1.1 concurrency: JS =="; check_conc "js" "js" "$JS_CONC"

# ── v1.1: durable cron (jobs.cron) ──────────────────────────────────────────
# cron_next math on a fixed epoch; a due schedule fires exactly one job; a
# second tick at the same time does NOT double-fire (CAS + missed-tick skip);
# uncron removes it. Uses the _tick/_cronNext test seams for determinism.
check_cron() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"; printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"CRON math=1 fired=1 nodup=1 uncron=1"*)
            pass "$label: cron (math / fire / no-double-fire / uncron)" ;;
        *) fail "$label: v1.1 cron" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_CRON='local jobs = require("hull.jobs")
local time = require("hull.time")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  local b = 1609459200   -- 2021-01-01 00:00:00 UTC
  local math_ok = (jobs._cron_next("*/15 * * * *",b)-b==900
    and jobs._cron_next("0 0 * * *",b)-b==86400
    and jobs._cron_next("30 2 * * *",b)-b==9000) and 1 or 0
  local fired = 0
  jobs.handler("beep", function(j) fired = fired + 1 end)
  jobs.cron("beep", "* * * * *")
  jobs._tick(time.now()+120); jobs.work({batch=10})
  local f1 = fired
  jobs._tick(time.now()+120); jobs.work({batch=10})   -- must not double-fire
  local nodup = (fired == f1) and 1 or 0
  local uncron = jobs.uncron("beep") and 1 or 0
  ctx.stdout:write(("CRON math=%d fired=%d nodup=%d uncron=%d\n"):format(math_ok, f1, nodup, uncron))
  return 0
end)'

JS_CRON='import { app } from "hull:app"; import { jobs } from "hull:jobs"; import { time } from "hull:time";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  const b = 1609459200;
  const math_ok = (jobs._cronNext("*/15 * * * *",b)-b===900
    && jobs._cronNext("0 0 * * *",b)-b===86400
    && jobs._cronNext("30 2 * * *",b)-b===9000) ? 1 : 0;
  let fired = 0;
  jobs.handler("beep", (j) => { fired++; });
  jobs.cron("beep", "* * * * *");
  jobs._tick(time.now()+120); await jobs.work({batch:10});
  const f1 = fired;
  jobs._tick(time.now()+120); await jobs.work({batch:10});
  const nodup = (fired === f1) ? 1 : 0;
  const uncron = jobs.uncron("beep") ? 1 : 0;
  ctx.stdout.write(`CRON math=${math_ok} fired=${f1} nodup=${nodup} uncron=${uncron}\n`);
  return 0;
});'

echo "== v1.1 cron: Lua =="; check_cron "lua" "lua" "$LUA_CRON"
echo "== v1.1 cron: JS =="; check_cron "js" "js" "$JS_CRON"

# ── v1.1: jobs.get(id) + jobs.heartbeat(job) ────────────────────────────────
# get() returns a job's status view (pending -> done; nil for a missing id).
# heartbeat() extends a held claim (true) and reports a lost claim (false) once
# the reaper has reclaimed it - the handler's anti-double-run signal.
check_gethb() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"; printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"GH get_ok=1 hb_held=1 hb_lost=1"*)
            pass "$label: get(id) status view + heartbeat claim-guard" ;;
        *) fail "$label: v1.1 get/heartbeat" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_GH='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  jobs.handler("t", function(j) return end)
  local id = jobs.enqueue("t", { x = 7 })
  local g0 = jobs.get(id)
  jobs.work({ batch = 10 })
  local g1 = jobs.get(id)
  local get_ok = (g0.status=="pending" and g0.data.x==7 and g1.status=="done" and jobs.get(999999)==nil) and 1 or 0
  jobs.enqueue("t", {})
  local job = jobs.claim({ batch = 1 })[1]
  local held = jobs.heartbeat(job) and 1 or 0
  jobs.reap({ visibility_timeout = 0 })
  local lost = (not jobs.heartbeat(job)) and 1 or 0
  ctx.stdout:write(("GH get_ok=%d hb_held=%d hb_lost=%d\n"):format(get_ok, held, lost))
  return 0
end)'

JS_GH='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  jobs.handler("t", (j) => {});
  const id = jobs.enqueue("t", { x: 7 });
  const g0 = jobs.get(id);
  await jobs.work({ batch: 10 });
  const g1 = jobs.get(id);
  const get_ok = (g0.status==="pending" && g0.data.x===7 && g1.status==="done" && jobs.get(999999)===null) ? 1 : 0;
  jobs.enqueue("t", {});
  const job = jobs.claim({ batch: 1 })[0];
  const held = jobs.heartbeat(job) ? 1 : 0;
  jobs.reap({ visibilityTimeout: 0 });
  const lost = (!jobs.heartbeat(job)) ? 1 : 0;
  ctx.stdout.write(`GH get_ok=${get_ok} hb_held=${held} hb_lost=${lost}\n`);
  return 0;
});'

echo "== v1.1 get/heartbeat: Lua =="; check_gethb "lua" "lua" "$LUA_GH"
echo "== v1.1 get/heartbeat: JS =="; check_gethb "js" "js" "$JS_GH"

# ── v1.2: fleet-wide rate limiting (jobs.limit) ─────────────────────────────
# rate=3 per long window: first claim yields 3, second yields 0 (budget spent),
# and a rate-deferred (requeued) job keeps attempts=0 (the claim was undone).
check_rl() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"; printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"RL first=3 second=0 attempts=0"*)
            pass "$label: rate limit (window budget + requeue keeps attempts)" ;;
        *) fail "$label: v1.2 rate limit" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_RL='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  jobs.limit("default", { rate = 3, per = 100 })
  for i=1,10 do jobs.enqueue("t", {}) end
  local c1 = jobs.claim({ batch = 10 })
  local c2 = jobs.claim({ batch = 10 })
  local at = -1
  for id=1,10 do local g=jobs.get(id); if g and g.status=="pending" then at=g.attempts; break end end
  ctx.stdout:write(("RL first=%d second=%d attempts=%d\n"):format(#c1, #c2, at))
  return 0
end)'

JS_RL='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  jobs.limit("default", { rate: 3, per: 100 });
  for (let i=0;i<10;i++) jobs.enqueue("t", {});
  const c1 = jobs.claim({ batch: 10 });
  const c2 = jobs.claim({ batch: 10 });
  let at = -1;
  for (let id=1; id<=10; id++) { const g=jobs.get(id); if (g && g.status==="pending") { at=g.attempts; break; } }
  ctx.stdout.write(`RL first=${c1.length} second=${c2.length} attempts=${at}\n`);
  return 0;
});'

echo "== v1.2 rate limit: Lua =="; check_rl "lua" "lua" "$LUA_RL"
echo "== v1.2 rate limit: JS =="; check_rl "js" "js" "$JS_RL"

# ── v1.3: queue pause / resume / purge ──────────────────────────────────────
# A paused queue is not claimed; resume restores it; purge deletes pending.
check_pq() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"; printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"PQ paused=0 resumed=5 purged=3 after_purge=0"*)
            pass "$label: queue pause / resume / purge" ;;
        *) fail "$label: v1.3 pause/resume/purge" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_PQ='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  for i=1,5 do jobs.enqueue("t", {}) end
  jobs.pause("default"); local c1 = jobs.claim({ batch = 10 })
  jobs.resume("default"); local c2 = jobs.claim({ batch = 10 })
  for i=1,3 do jobs.enqueue("t", {}) end
  local purged = jobs.purge("default"); local c3 = jobs.claim({ batch = 10 })
  ctx.stdout:write(("PQ paused=%d resumed=%d purged=%d after_purge=%d\n"):format(#c1, #c2, purged, #c3))
  return 0
end)'

JS_PQ='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  for (let i=0;i<5;i++) jobs.enqueue("t", {});
  jobs.pause("default"); const c1 = jobs.claim({ batch: 10 });
  jobs.resume("default"); const c2 = jobs.claim({ batch: 10 });
  for (let i=0;i<3;i++) jobs.enqueue("t", {});
  const purged = jobs.purge("default"); const c3 = jobs.claim({ batch: 10 });
  ctx.stdout.write(`PQ paused=${c1.length} resumed=${c2.length} purged=${purged} after_purge=${c3.length}\n`);
  return 0;
});'

echo "== v1.3 pause/resume/purge: Lua =="; check_pq "lua" "lua" "$LUA_PQ"
echo "== v1.3 pause/resume/purge: JS =="; check_pq "js" "js" "$JS_PQ"

# ── v1.4: workflows (depends_on) - chain, result-passing, cascade, fan-in ───
# extract->transform->load chain passes results via job.deps; a failed dep
# cascade-fails its dependent (on_dep_failure="run" opts out); fan-in gets both.
check_wf() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"; printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"WF blocked=1 t=10 l=20 cascade=dead runaway=done ran=1 fanin=done m1=10 m2=10"*)
            pass "$label: workflows (chain/result/cascade/run-anyway/fan-in)" ;;
        *) fail "$label: v1.4 workflows" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_WF='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  local L = {}
  jobs.handler("extract",   function(j) return { v = 10 } end)
  jobs.handler("transform", function(j) L.t = j.deps[1].v; return { v = j.deps[1].v * 2 } end)
  jobs.handler("load",      function(j) L.l = j.deps[1].v end)
  jobs.handler("boom",      function(j) error("fail") end)
  jobs.handler("child",     function(j) end)
  jobs.handler("run_anyway",function(j) L.ran = true end)
  jobs.handler("merge",     function(j) L.m1 = j.deps[1].v; L.m2 = j.deps[2].v end)
  local a=jobs.enqueue("extract",{}); local b=jobs.enqueue("transform",{},{depends_on={a}})
  local c=jobs.enqueue("load",{},{depends_on={b}}); local b0=jobs.get(b).status
  local x=jobs.enqueue("boom",{},{max_attempts=1}); local y=jobs.enqueue("child",{},{depends_on={x}})
  local p=jobs.enqueue("boom",{},{max_attempts=1}); local q=jobs.enqueue("run_anyway",{},{depends_on={p},on_dep_failure="run"})
  local e1=jobs.enqueue("extract",{}); local e2=jobs.enqueue("extract",{}); local m=jobs.enqueue("merge",{},{depends_on={e1,e2}})
  jobs.run_worker({ drain = true, poll_ms = 5 })
  ctx.stdout:write(("WF blocked=%d t=%s l=%s cascade=%s runaway=%s ran=%d fanin=%s m1=%s m2=%s\n"):format(
    b0=="blocked" and 1 or 0, tostring(L.t), tostring(L.l), jobs.get(y).status, jobs.get(q).status,
    L.ran and 1 or 0, jobs.get(m).status, tostring(L.m1), tostring(L.m2)))
  return 0
end)'

JS_WF='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  const L = {};
  jobs.handler("extract",   (j) => ({ v: 10 }));
  jobs.handler("transform", (j) => { L.t = j.deps[0].v; return { v: j.deps[0].v * 2 }; });
  jobs.handler("load",      (j) => { L.l = j.deps[0].v; });
  jobs.handler("boom",      (j) => { throw new Error("fail"); });
  jobs.handler("child",     (j) => {});
  jobs.handler("run_anyway",(j) => { L.ran = true; });
  jobs.handler("merge",     (j) => { L.m1 = j.deps[0].v; L.m2 = j.deps[1].v; });
  const a=jobs.enqueue("extract",{}); const b=jobs.enqueue("transform",{},{dependsOn:[a]});
  const c=jobs.enqueue("load",{},{dependsOn:[b]}); const b0=jobs.get(b).status;
  const x=jobs.enqueue("boom",{},{maxAttempts:1}); const y=jobs.enqueue("child",{},{dependsOn:[x]});
  const p=jobs.enqueue("boom",{},{maxAttempts:1}); const q=jobs.enqueue("run_anyway",{},{dependsOn:[p],onDepFailure:"run"});
  const e1=jobs.enqueue("extract",{}); const e2=jobs.enqueue("extract",{}); const m=jobs.enqueue("merge",{},{dependsOn:[e1,e2]});
  await jobs.runWorker({ drain: true, pollMs: 5 });
  ctx.stdout.write(`WF blocked=${b0==="blocked"?1:0} t=${L.t} l=${L.l} cascade=${jobs.get(y).status} runaway=${jobs.get(q).status} ran=${L.ran?1:0} fanin=${jobs.get(m).status} m1=${L.m1} m2=${L.m2}\n`);
  return 0;
});'

echo "== v1.4 workflows: Lua =="; check_wf "lua" "lua" "$LUA_WF"
echo "== v1.4 workflows: JS =="; check_wf "js" "js" "$JS_WF"

# ── v1.5: jobs.result(id) + jobs.await(id) - the standalone result backend ──
# A handler's non-nil return is readable via jobs.result (done->value,
# dead->error, pending->neither, unknown->nil); jobs.await yields until terminal.
check_result() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"; printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"RES pre=pending done=7 dead=boom void=nil unknown=nil await=7"*)
            pass "$label: result/await (done-value / dead-error / void / unknown / yielding await)" ;;
        *) fail "$label: v1.5 result/await" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_RES='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  jobs.handler("sum",  function(j) return { total = j.data.a + j.data.b } end)
  jobs.handler("boom", function(j) error("boom") end)
  jobs.handler("void", function(j) return end)
  local ok=jobs.enqueue("sum",{a=2,b=5}); local bad=jobs.enqueue("boom",{},{max_attempts=1}); local vd=jobs.enqueue("void",{})
  local pre=jobs.result(ok).status
  jobs.work({ batch = 10 })
  local r1=jobs.result(ok); local r2=jobs.result(bad); local r3=jobs.result(vd); local r4=jobs.result(999999)
  local aw=jobs.await(ok, { timeout = 2000 })
  local derr = tostring(r2.error):match("boom") and "boom" or "?"
  ctx.stdout:write(("RES pre=%s done=%s dead=%s void=%s unknown=%s await=%s\n"):format(
    pre, tostring(r1.result.total), derr, tostring(r3.result), tostring(r4), tostring(aw.result.total)))
  return 0
end)'

JS_RES='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
  jobs.init();
  jobs.handler("sum",  (j) => ({ total: j.data.a + j.data.b }));
  jobs.handler("boom", (j) => { throw new Error("boom"); });
  jobs.handler("void", (j) => {});
  const ok=jobs.enqueue("sum",{a:2,b:5}); const bad=jobs.enqueue("boom",{},{maxAttempts:1}); const vd=jobs.enqueue("void",{});
  const pre=jobs.result(ok).status;
  await jobs.work({ batch: 10 });
  const r1=jobs.result(ok), r2=jobs.result(bad), r3=jobs.result(vd), r4=jobs.result(999999);
  const aw=await jobs.await(ok, { timeout: 2000 });
  const derr = /boom/.test(String(r2.error)) ? "boom" : "?";
  ctx.stdout.write(`RES pre=${pre} done=${r1.result.total} dead=${derr} void=${r3.result === undefined ? "nil" : r3.result} unknown=${r4 === null ? "nil" : r4} await=${aw.result.total}\n`);
  return 0;
});'

echo "== v1.5 result/await: Lua =="; check_result "lua" "lua" "$LUA_RES"
echo "== v1.5 result/await: JS =="; check_result "js" "js" "$JS_RES"

# ── v1.5: multi-queue draining - strict-priority list + weighted map ────────
# A list = strict priority (critical drained before default before low); a map =
# weighted fairness (every queue drains, no starvation).
check_queues() {
    label="$1"; ext="$2"; app="$3"
    T="$(mktemp -d)"; printf '%s\n' "$app" > "$T/app.$ext"
    out="$("$HULL" "$T/app.$ext" -d "$T/a.db" 2>/dev/null)" || true
    case "$out" in
        *"STRICT order=c1,c2,d1,l1 WEIGHTED drained=20"*)
            pass "$label: multi-queue (strict-priority list + weighted-map no-starvation)" ;;
        *) fail "$label: v1.5 multi-queue draining" "$out" ;;
    esac
    rm -rf "$T"
}

LUA_QUEUES='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  jobs.enqueue("t",{n="c1"},{queue="critical"}); jobs.enqueue("t",{n="c2"},{queue="critical"})
  jobs.enqueue("t",{n="d1"},{queue="default"});  jobs.enqueue("t",{n="l1"},{queue="low"})
  local order = {}
  while true do
    local b = jobs.claim({ queues = {"critical","default","low"}, batch = 1 })
    if #b == 0 then break end
    order[#order+1] = b[1].data.n
  end
  for i=1,10 do jobs.enqueue("t",{},{queue="A"}); jobs.enqueue("t",{},{queue="B"}) end
  local drained, safety = 0, 0
  while safety < 200 do
    local b = jobs.claim({ queues = {A=3, B=1}, batch = 3 })
    if #b == 0 then break end
    drained = drained + #b; safety = safety + 1
  end
  ctx.stdout:write(("STRICT order=%s WEIGHTED drained=%d\n"):format(table.concat(order,","), drained))
  return 0
end)'

JS_QUEUES='import { app } from "hull:app"; import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main((ctx) => {
  jobs.init();
  jobs.enqueue("t",{n:"c1"},{queue:"critical"}); jobs.enqueue("t",{n:"c2"},{queue:"critical"});
  jobs.enqueue("t",{n:"d1"},{queue:"default"});  jobs.enqueue("t",{n:"l1"},{queue:"low"});
  const order = [];
  for (;;) { const b = jobs.claim({ queues: ["critical","default","low"], batch: 1 }); if (!b.length) break; order.push(b[0].data.n); }
  for (let i=0;i<10;i++){ jobs.enqueue("t",{},{queue:"A"}); jobs.enqueue("t",{},{queue:"B"}); }
  let drained=0, safety=0;
  while (safety<200){ const b=jobs.claim({ queues:{A:3,B:1}, batch:3 }); if(!b.length) break; drained+=b.length; safety++; }
  ctx.stdout.write(`STRICT order=${order.join(",")} WEIGHTED drained=${drained}\n`);
  return 0;
});'

echo "== v1.5 multi-queue: Lua =="; check_queues "lua" "lua" "$LUA_QUEUES"
echo "== v1.5 multi-queue: JS =="; check_queues "js" "js" "$JS_QUEUES"

# Fleet gate: K processes share one rate counter -> total dispatched == rate.
echo "== v1.2 rate limit fleet ($CONC processes, one shared counter) =="
W="$(mktemp -d)"; DB="$W/rl.db"
cat > "$W/seed.lua" <<LUA
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function() jobs.init(); jobs.limit("default",{rate=5,per=100}); for i=1,50 do jobs.enqueue("t",{}) end; return 0 end)
LUA
cat > "$W/claim.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init(); jobs.limit("default",{rate=5,per=100})
  while true do
    local b = jobs.claim({ batch = 2 })
    if #b == 0 then break end
    for _,j in ipairs(b) do ctx.stdout:write(j.id .. "\n") end
  end
  return 0
end)
LUA
"$HULL" "$W/seed.lua" -d "$DB" >/dev/null 2>&1
i=1; while [ "$i" -le "$CONC" ]; do "$HULL" "$W/claim.lua" -d "$DB" > "$W/out.$i" 2>/dev/null & i=$((i+1)); done
wait
total="$(cat "$W"/out.* | grep -c . || true)"
uniq="$(cat "$W"/out.* | sort -n | uniq | grep -c . || true)"
if [ "$total" -eq 5 ] && [ "$uniq" -eq 5 ]; then
    pass "rate limit fleet: $CONC processes dispatched exactly 5 (the shared budget)"
else
    fail "rate limit fleet: expected 5 total" "total=$total uniq=$uniq"
fi
rm -rf "$W"

echo ""
echo "=== Summary ==="
echo "PASSED: $PASS"
echo "FAILED: $FAIL"
[ "$FAIL" -eq 0 ]
