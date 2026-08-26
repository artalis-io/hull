#!/usr/bin/env python3
# test_job_plan.py - fixtures for the job-applicability map. Proves
# run-flags + gate allow-skip for: docs-only, pure JS, pure Lua, fuzz-only, core,
# mixed, main, force-full, unexpected skip, and missing applicability.
# Run: python3 scripts/ci/test_job_plan.py
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classify_changes import classify, PLAN  # noqa: E402
import job_plan  # noqa: E402

ALL_JOBS = sorted(job_plan.GROUP.keys())
_pass = 0
_fail = 0


def plan_for(paths, event="pull_request", force_full=False):
    return classify(list(paths), event=event, force_full=force_full)["plan"]


def full_plan(**overrides):
    """A COMPLETE canonical plan (the exact PLAN schema, all-False + lint) with
    overrides applied. Hand-built partial dicts are now rejected as malformed
    (_well_formed_plan requires the exact schema), so narrow-positive fixtures
    must build a full plan the way the classifier does."""
    p = {k: False for k in PLAN}
    p["lint"] = True
    p.update(overrides)
    return p


def check(name, cond):
    global _pass, _fail
    if cond:
        _pass += 1
    else:
        _fail += 1
        print("FAIL:", name)


def flags(paths, **kw):
    return job_plan.run_flags(plan_for(paths, **kw))


def skips(paths, event="pull_request", **kw):
    return job_plan.allow_skip_jobs(plan_for(paths, event=event, **kw), ALL_JOBS, event)


# -- docs-only: nothing but always-on runs; everything else may skip --
f = flags(["docs/x.md"])
check("docs-only: no group runs", not any(f.values()))
s = skips(["docs/x.md"])
check("docs-only: build may skip", "build" in s)
check("docs-only: focused-js may skip", "focused-js-frontend" in s)
check("docs-only: fuzz-core-security may skip", "fuzz-core-security" in s)
check("docs-only: lint never skips", "lint" not in s)
check("docs-only: classify never skips", "classify" not in s)

# -- pure JS frontend: focused-js + fuzz-js run; core-common + native + lua skip --
f = flags(["stdlib/cli/js/hull/source/parser.js"])
check("pure-js: run_focused_js", f["run_focused_js"])
check("pure-js: run_fuzz_js", f["run_fuzz_js"])
check("pure-js: NOT run_core_common (frontend narrow)", not f["run_core_common"])
check("pure-js: NOT run_focused_lua", not f["run_focused_lua"])
s = skips(["stdlib/cli/js/hull/source/parser.js"])
check("pure-js: build may skip", "build" in s)
check("pure-js: focused-lua may skip", "focused-lua-frontend" in s)
check("pure-js: fuzz-core-security may skip", "fuzz-core-security" in s)
check("pure-js: focused-js NOT skippable", "focused-js-frontend" not in s)
check("pure-js: fuzz-js NOT skippable", "fuzz-js-source" not in s)

# -- pure Lua frontend --
f = flags(["stdlib/cli/lua/hull/source/lexer.lua"])
check("pure-lua: run_focused_lua", f["run_focused_lua"])
check("pure-lua: run_fuzz_lua", f["run_fuzz_lua"])
check("pure-lua: NOT run_core_common (frontend narrow)", not f["run_core_common"])
s = skips(["stdlib/cli/lua/hull/source/lexer.lua"])
check("pure-lua: focused-lua NOT skippable", "focused-lua-frontend" not in s)
check("pure-lua: build may skip", "build" in s)

# -- fuzz-only: a source-parser fuzzer .c sets frontend+fuzz -> narrow --
f = flags(["fuzz/fuzz_js_source.c"])
check("js-fuzz .c: run_fuzz_js + focused_js", f["run_fuzz_js"] and f["run_focused_js"])
check("js-fuzz .c: NOT run_core_common", not f["run_core_common"])
# a NATIVE-protocol fuzzer .c is NOT a proven narrow class -> broad
f = flags(["fuzz/fuzz_pgwire.c"])
check("native-fuzz .c -> broad", all(f.values()))

# -- core (a genuinely non-allowlisted production file): broad, everything runs --
f = flags(["src/hull/serve.c"])
check("core: broad (all group flags true)", all(f.values()))
check("core: nothing skippable except benchmark(non-push)",
      skips(["src/hull/serve.c"]) == {"benchmark"})

# -- mixed core + frontend: a core file forces broad --
f = flags(["stdlib/cli/js/hull/source/parser.js", "src/hull/serve.c"])
check("mixed core+frontend: broad", all(f.values()))
check("mixed: run_focused_js", f["run_focused_js"])
check("mixed: build NOT skippable",
      "build" not in skips(["stdlib/cli/js/hull/source/parser.js", "src/hull/serve.c"]))

# -- main push / force-full: everything runs regardless of paths --
f = flags(["docs/x.md"], event="push_main")
check("push_main: broad (all groups)", all(f.values()))
check("push_main: nothing force-skippable (event=push => none)",
      job_plan.allow_skip_jobs(plan_for(["docs/x.md"], event="push_main"), ALL_JOBS, "push") == set())
f = flags(["docs/x.md"], force_full=True)
check("force-full: broad (all groups)", all(f.values()))

# -- unexpected skip: build applicable in a broad plan -> NOT in allow-skip --
check("broad: build must run (not in allow-skip)",
      "build" not in job_plan.allow_skip_jobs(plan_for(["src/hull/serve.c"]), ALL_JOBS, "pull_request"))

# -- missing applicability: an UNMAPPED job defaults to always -> never skips --
s = job_plan.allow_skip_jobs(plan_for(["docs/x.md"]), ALL_JOBS + ["brand_new_job"], "pull_request")
check("unmapped job defaults applicable (never skippable)", "brand_new_job" not in s)
check("but a mapped core-common job IS skippable in docs-only", "build" in s)

# -- benchmark: push-only; skippable on PR regardless of plan, NOT on push --
check("benchmark skippable on PR (broad plan)",
      "benchmark" in job_plan.allow_skip_jobs(plan_for(["src/hull/serve.c"]), ALL_JOBS, "pull_request"))
check("benchmark NOT skippable on push",
      "benchmark" not in job_plan.allow_skip_jobs(plan_for([], event="push_main"), ALL_JOBS, "push"))

# -- fail-closed: the positive narrow allowlist. Anything not positively narrow
#    (empty, non-dict, non-boolean, unknown, or a non-approved true flag) -> BROAD --
def is_broad(plan):
    return all(job_plan.run_flags(plan).values())

check("empty plan {} -> broad", is_broad({}))
check("non-dict (list) plan -> broad", is_broad([]))
check("unknown true flag -> broad", is_broad({"lint": True, "focused_js_frontend": True, "bogus_flag": True}))
check("string-valued flag -> broad", is_broad({"lint": True, "focused_js_frontend": "true"}))
check("int-valued flag -> broad", is_broad({"lint": True, "focused_js_frontend": 1}))
check("lint-only (no narrow selector) -> broad", is_broad({"lint": True}))
check("full_all present -> broad", is_broad({"lint": True, "full_all": True}))
check("full_core present -> broad", is_broad({"lint": True, "full_core": True}))
# a genuinely-approved COMPLETE narrow plan still narrows (positive path works)
check("approved narrow (docs_only) -> not broad", not is_broad(full_plan(docs_only=True)))
check("approved narrow (js) -> focused-js only, no core-common",
      job_plan.run_flags(full_plan(focused_js_frontend=True))["run_focused_js"]
      and not job_plan.run_flags(full_plan(focused_js_frontend=True))["run_core_common"])

# ---- native narrowing ACTIVE (skipping on) -------------
# An isolated native change is now NARROW: it runs the core-common floor + its
# subsystem (+ the db-any umbrella for a db change) and SKIPS the other
# subsystems, the broad-only web group, and the frontend groups. A native change
# mixed with any non-approved selector falls closed to BROAD (constraints below).

# narrow-native (db_postgres): core-common + db-postgres + db-any run; rest skip.
f = flags(["src/hull/cap/db_postgres.c"])
check("db_postgres NARROW: core-common on", f["run_core_common"])
check("db_postgres NARROW: db-postgres + db-any on", f["run_db_postgres"] and f["run_db_any"])
check("db_postgres NARROW: gpu/compute/web/other-db OFF",
      not any(f[k] for k in ("run_gpu", "run_compute", "run_web",
                             "run_db_mysql", "run_db_valkey", "run_db_duckdb")))
check("db_postgres NARROW: frontend OFF",
      not any(f[k] for k in ("run_focused_js", "run_focused_lua", "run_fuzz_js", "run_fuzz_lua")))
_s = skips(["src/hull/cap/db_postgres.c"])
check("db_postgres NARROW: gpu jobs skippable", {"gpu", "gpu-feature"} <= _s)
check("db_postgres NARROW: other-backend + compute + web skippable",
      {"mysql", "valkey", "duckdb", "wasm-readonly-heap-aot",
       "htmx-browser", "project-discovery-lua"} <= _s)
check("db_postgres NARROW: core-common jobs NOT skippable",
      not ({"build", "sanitizers", "reproducibility", "fuzz-core-security"} & _s))
check("db_postgres NARROW: postgres + fuzz-db-wire NOT skippable",
      not ({"postgres", "postgres-feature", "fuzz-db-wire"} & _s))

# gpu-only and compute-only narrow.
fg = flags(["src/hull/cap/gpu_wgpu.c"])
check("gpu NARROW: core-common + gpu on, db/compute off",
      fg["run_core_common"] and fg["run_gpu"] and not fg["run_db_any"] and not fg["run_compute"])
fc = flags(["src/hull/cap/wasm.c"])
check("compute NARROW: core-common + compute on, db/gpu off",
      fc["run_core_common"] and fc["run_compute"] and not fc["run_db_any"] and not fc["run_gpu"])

# native_groups() = the live subsystem mapping consulted by applicable_groups.
def ng(paths):
    return job_plan.native_groups(plan_for(paths))

check("ng db_postgres -> core-common + db-postgres + db-any",
      ng(["src/hull/cap/db_postgres.c"]) == {"core-common", "db-postgres", "db-any"})
check("ng gpu -> core-common + gpu",
      ng(["src/hull/cap/gpu_wgpu.c"]) == {"core-common", "gpu"})
check("ng wasm -> core-common + compute",
      ng(["src/hull/cap/wasm.c"]) == {"core-common", "compute"})
check("ng shared-db -> core-common + all four sub-groups + db-any",
      ng(["src/hull/cap/db_select.c"]) ==
      {"core-common", "db-postgres", "db-mysql", "db-valkey", "db-duckdb", "db-any"})
# additive: DB + GPU -> both sets.
check("ng additive (gpu + db_postgres)",
      ng(["src/hull/cap/gpu.c", "src/hull/cap/db_postgres.c"]) ==
      {"core-common", "db-postgres", "db-any", "gpu"})
# two backends -> both sub-groups + ONE db-any umbrella (Amendment 1).
check("ng two-backend -> both sub-groups + single db-any",
      ng(["src/hull/cap/db_postgres.c", "src/hull/cap/db_mysql.c"]) ==
      {"core-common", "db-postgres", "db-mysql", "db-any"})
# broad / malformed / docs -> fail closed / empty appropriately.
check("ng broad (core file) -> every native group",
      ng(["src/hull/serve.c"]) == set(job_plan.NATIVE_SUBSYSTEM_GROUPS))
check("ng malformed plan -> every native group (fail closed)",
      job_plan.native_groups({}) == set(job_plan.NATIVE_SUBSYSTEM_GROUPS)
      and job_plan.native_groups([]) == set(job_plan.NATIVE_SUBSYSTEM_GROUPS))
check("ng docs-only -> no native group",
      ng(["docs/x.md"]) == set())

# ---- CP3 constraint 1: native + any NON-approved selector -> BROAD -----------
# A native change mixed with tooling / project-discovery / query / tls / generic
# tests-or-examples / a future-or-unknown selector must fall closed to broad.
check("backend + tooling -> broad",
      is_broad(plan_for(["src/hull/cap/db_postgres.c", "stdlib/cli/lua/hull/deploy.lua"])))
check("backend + project-discovery -> broad",
      is_broad(plan_for(["src/hull/cap/db_postgres.c", "stdlib/cli/lua/hull/project/model.lua"])))
check("backend + generic test -> broad",
      is_broad(plan_for(["src/hull/cap/db_postgres.c", "tests/hull/cap/test_db.c"])))
check("backend + example -> broad",
      is_broad(plan_for(["src/hull/cap/db_postgres.c", "examples/todo/app.lua"])))
# plan-level: a COMPLETE, COHERENT plan whose native selector is mixed with a
# non-approved known flag (focused_query / focused_tls / focused_native_fuzz) ->
# broad because of the non-approved flag (not because of a schema defect).
check("plan: backend + focused_query -> broad",
      is_broad(full_plan(focused_db=True, focused_db_postgres=True, focused_query=True)))
check("plan: backend + focused_tls -> broad",
      is_broad(full_plan(focused_db=True, focused_db_postgres=True, focused_tls=True)))
check("plan: backend + focused_native_fuzz -> broad",
      is_broad(full_plan(focused_db=True, focused_db_postgres=True, focused_native_fuzz=True)))
# a FUTURE/unknown selector = an EXTRA key beyond the schema -> broad (exact-schema).
_future = full_plan(focused_db=True, focused_db_postgres=True)
_future["future_flag"] = True
check("plan: backend + FUTURE unknown flag -> broad", is_broad(_future))

# ---- CP3 fail-closed: exact schema + native-selector coherence ---------------
# A syntactically-valid but INCOMPLETE/INCOHERENT plan must go BROAD, never narrow
# to a partial subsystem set.
# valid-looking PARTIAL plan (coherent, but missing keys) -> broad.
check("partial plan (missing keys) -> broad",
      is_broad({"lint": True, "focused_db": True, "focused_db_postgres": True}))
# focused_db with NO backend selector -> incoherent -> broad.
check("focused_db without a backend -> broad", is_broad(full_plan(focused_db=True)))
# a backend selector with focused_db=False -> incoherent -> broad.
check("backend without focused_db -> broad", is_broad(full_plan(focused_db_postgres=True)))
# mismatched compute/wasm pairing -> broad (both directions).
check("focused_compute without focused_wasm -> broad", is_broad(full_plan(focused_compute=True)))
check("focused_wasm without focused_compute -> broad", is_broad(full_plan(focused_wasm=True)))
# the COMPLETE canonical plans continue to narrow correctly (positive path).
check("canonical docs_only narrows", not is_broad(full_plan(docs_only=True)))
check("canonical single-backend narrows",
      not is_broad(full_plan(focused_db=True, focused_db_postgres=True)))
check("canonical shared-db narrows",
      not is_broad(full_plan(focused_db=True, focused_db_postgres=True, focused_db_mysql=True,
                             focused_db_valkey=True, focused_db_duckdb=True)))
check("canonical compute (paired wasm+compute) narrows",
      not is_broad(full_plan(focused_compute=True, focused_wasm=True)))
check("canonical gpu narrows", not is_broad(full_plan(focused_gpu=True)))
# and the real classifier output for these paths still narrows (schema always exact).
check("classifier db_postgres plan still narrows", not is_broad(plan_for(["src/hull/cap/db_postgres.c"])))
check("classifier wasm plan still narrows", not is_broad(plan_for(["src/hull/cap/wasm.c"])))

# ---- CP3 constraint 2: native + approved frontend -> ADDITIVE ----------------
# core-common + native subsystem + the matching frontend/fuzzer groups; frontend
# groups are NOT discarded. (The broad-only web group stays off.)
fj = flags(["src/hull/cap/db_postgres.c", "stdlib/cli/js/hull/source/parser.js"])
check("backend + JS frontend: additive (core-common + db + js), web off",
      fj["run_core_common"] and fj["run_db_postgres"] and fj["run_db_any"]
      and fj["run_focused_js"] and fj["run_fuzz_js"]
      and not fj["run_web"] and not fj["run_gpu"] and not fj["run_compute"]
      and not fj["run_focused_lua"])
_sj = skips(["src/hull/cap/db_postgres.c", "stdlib/cli/js/hull/source/parser.js"])
check("backend + JS frontend: focused-js-frontend NOT skipped (not discarded)",
      "focused-js-frontend" not in _sj and "fuzz-js-source" not in _sj)
# DB + GPU + frontend -> all three additive.
fdgf = flags(["src/hull/cap/db_postgres.c", "src/hull/cap/gpu.c",
              "stdlib/cli/js/hull/source/parser.js"])
check("DB + GPU + JS frontend: additive (core-common + db + gpu + js)",
      fdgf["run_core_common"] and fdgf["run_db_postgres"] and fdgf["run_db_any"]
      and fdgf["run_gpu"] and fdgf["run_focused_js"] and fdgf["run_fuzz_js"]
      and not fdgf["run_compute"] and not fdgf["run_web"] and not fdgf["run_focused_lua"])

# the three split fuzz jobs now belong to their subsystem groups.
_FUZZ_GROUP = {"fuzz-core-security": "core-common", "fuzz-db-wire": "db-any",
               "fuzz-compute-span": "compute"}
for _j, _g in _FUZZ_GROUP.items():
    check("fuzz %s in group %s" % (_j, _g), job_plan.GROUP.get(_j) == _g)
    check("fuzz %s skippable on docs-only" % _j, _j in skips(["docs/x.md"]))
    check("fuzz %s runs on broad (not skippable)" % _j, _j not in skips(["src/hull/serve.c"]))
check("fuzz-db-wire runs on narrow-DB", "fuzz-db-wire" not in skips(["src/hull/cap/db_postgres.c"]))
check("fuzz-db-wire skips on gpu-only", "fuzz-db-wire" in skips(["src/hull/cap/gpu_wgpu.c"]))
check("fuzz-compute-span runs on compute change", "fuzz-compute-span" not in skips(["src/hull/cap/wasm.c"]))
check("fuzz-compute-span skips on db-only", "fuzz-compute-span" in skips(["src/hull/cap/db_postgres.c"]))

# the wamrc producer + verify jobs are compute-grouped - they run on a
# compute-applicable plan and skip (legitimately) only when compute is inapplicable.
for _j in ("wamrc-x86_64", "wamrc-artifact-verify"):
    check("5A %s in compute group" % _j, job_plan.GROUP.get(_j) == "compute")
    check("5A %s runs on compute change" % _j, _j not in skips(["src/hull/cap/wasm.c"]))
    check("5A %s runs on broad" % _j, _j not in skips(["src/hull/serve.c"]))
    check("5A %s skips on db-only (not compute)" % _j, _j in skips(["src/hull/cap/db_postgres.c"]))
    check("5A %s skips on docs-only" % _j, _j in skips(["docs/x.md"]))

# fuzz coverage-equivalence: the union of the three jobs' build targets in ci.yml
# equals the former single fuzz-native-security set (no target dropped in the split).
import re  # noqa: E402
CANONICAL_FUZZ = {
    "sh_json", "path_normalize", "mime_sniff", "host_match",          # core
    "pgwire", "pg_dsn", "pg_rewrite", "mysqlwire", "mysql_dsn", "respwire", "valkey_dsn",  # db wire
    "span_sdk", "span_window",                                        # compute
}
_WF = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                   ".github", "workflows", "ci.yml")
_wf_lines = open(_WF, encoding="utf-8").read().splitlines()


def _job_slice(job):
    out, cur = [], False
    for ln in _wf_lines:
        m = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", ln)
        if m:
            cur = (m.group(1) == job)
            continue
        if cur:
            out.append(ln)
    return "\n".join(out)


_split_targets, _overlap = set(), []
for _j in ("fuzz-core-security", "fuzz-db-wire", "fuzz-compute-span"):
    _t = set(re.findall(r"fuzz/fuzz_([a-z0-9_]+)", _job_slice(_j)))
    if _split_targets & _t:
        _overlap.append(_j)
    _split_targets |= _t
check("fuzz_union: split targets == canonical set", _split_targets == CANONICAL_FUZZ)
check("fuzz_union: sub-jobs are disjoint (no target in two jobs)", not _overlap)

print("job_plan fixtures: %d passed, %d failed" % (_pass, _fail))
sys.exit(1 if _fail else 0)
