# shellcheck shell=sh
# hull_jobs_audit38.sh - the audit 38 jobs apps shared by e2e_postgres.sh and
# e2e_mysql.sh: a handler error / subscriber error / job result holding text
# the server refuses (a NUL, bytes that are not UTF-8, a lone surrogate, more
# than 64 KiB) must leave the job, its batch-mates and the worker in a
# consistent state. Each app prints one line:
#   A38 work=true/true p=pending a=done b=done r=dead safe=1 skip=1 bin=1
# work  - neither jobs.work call raised / threw
# p     - the poison job was re-pended (its retry written)
# a, b  - its batch-mates ran
# r     - the job whose result cannot be stored (cannot be encoded) was
#         dead-lettered once
# bin   - a result holding bytes that are not UTF-8 (Lua) / a lone surrogate
#         and a NUL (JS) completed and reads back equal (audit 39: round 38
#         dead-lettered it, on SQLite too)
# safe  - the stored last_error is storable text (no NUL, capped)
# skip  - the poison subscription event was skipped, with a record
#
# jobs_audit38_apps DIR  writes DIR/a38.lua and DIR/a38.js.

jobs_audit38_apps() {
    cat > "$1/a38.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init({ events = true, events_grace = 0, history = true,
              backoff = function() return 0 end })
  jobs.handler("a38p", function()
    error("upstream: a\0b caf\xc3 " .. string.rep("y", 100000), 0)
  end)
  jobs.handler("a38ok", function() return true end)
  jobs.handler("a38r", function() return { f = function() end } end)
  local BIN = "\255\254\0abc caf\xc3"
  jobs.handler("a38b", function() return { d = BIN } end)
  local p = jobs.enqueue("a38p", {}, { queue = "a38" })
  local a = jobs.enqueue("a38ok", {}, { queue = "a38" })
  local b = jobs.enqueue("a38ok", {}, { queue = "a38" })
  local r = jobs.enqueue("a38r", {}, { queue = "a38" })
  local bj = jobs.enqueue("a38b", {}, { queue = "a38" })
  local ok1 = pcall(jobs.work, { queue = "a38", batch = 10 })
  jobs.subscribe("a38s", function(ev)
    if ev.type == "dead" then error("forward failed: caf\xc3\0", 0) end
  end, { from = "beginning", max_failures = 1 })
  local ok2 = pcall(jobs.work, { queue = "a38none" })   -- drives the drain
  local le = jobs.get(p).last_error or ""
  local safe = (not le:find("\0", 1, true)) and #le <= 4099 and #le > 0
  local br = jobs.result(bj)
  local bin = br and br.status == "done" and br.result and br.result.d == BIN
  ctx.stdout:write(("A38 work=%s/%s p=%s a=%s b=%s r=%s safe=%d skip=%d bin=%d\n"):format(
    tostring(ok1), tostring(ok2), jobs.get(p).status, jobs.get(a).status,
    jobs.get(b).status, jobs.get(r).status, safe and 1 or 0,
    #jobs.events({ types = { "subscription_skipped" } }), bin and 1 or 0))
  return 0
end)
LUA
    cat > "$1/a38.js" <<'JS'
import { app } from "hull:app";
import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main(async (ctx) => {
    jobs.init({ events: true, eventsGrace: 0, history: true, backoff: () => 0 });
    jobs.handler("a38p", () => {
        throw new Error("upstream: a\u0000b " + "ok \u{1F600}".slice(0, 4) + " " + "y".repeat(100000));
    });
    jobs.handler("a38ok", () => true);
    jobs.handler("a38r", () => { const o = {}; o.self = o; return o; });
    const BIN = "\ud800x\u0000y";
    jobs.handler("a38b", () => ({ d: BIN }));
    const p = jobs.enqueue("a38p", {}, { queue: "a38" });
    const a = jobs.enqueue("a38ok", {}, { queue: "a38" });
    const b = jobs.enqueue("a38ok", {}, { queue: "a38" });
    const r = jobs.enqueue("a38r", {}, { queue: "a38" });
    const bj = jobs.enqueue("a38b", {}, { queue: "a38" });
    let ok1 = true, ok2 = true;
    try { await jobs.work({ queue: "a38", batch: 10 }); } catch (_e) { ok1 = false; }
    jobs.subscribe("a38s", (ev) => {
        if (ev.type === "dead") throw new Error("forward failed: " + "\u{1F600}".slice(0, 1) + "\u0000");
    }, { from: "beginning", maxFailures: 1 });
    try { await jobs.work({ queue: "a38none" }); } catch (_e) { ok2 = false; }   // drives the drain
    const le = jobs.get(p).lastError || "";
    const safe = !le.includes("\u0000") && le.length <= 4099 && le.length > 0;
    ctx.stdout.write(`A38 work=${ok1}/${ok2} p=${jobs.get(p).status} a=${jobs.get(a).status} ` +
        `b=${jobs.get(b).status} r=${jobs.get(r).status} safe=${safe ? 1 : 0} ` +
        `skip=${jobs.events({ types: ["subscription_skipped"] }).length} ` +
        `bin=${(() => { const x = jobs.result(bj); return x && x.status === "done" && x.result && x.result.d === BIN ? 1 : 0; })()}\n`);
    return 0;
});
JS
}
