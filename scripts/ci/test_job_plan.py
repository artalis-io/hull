#!/usr/bin/env python3
# test_job_plan.py - fixtures for the job-applicability map (Slice 3b). Proves
# run-flags + gate allow-skip for: docs-only, pure JS, pure Lua, fuzz-only, core,
# mixed, main, force-full, unexpected skip, and missing applicability.
# Run: python3 scripts/ci/test_job_plan.py
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classify_changes import classify  # noqa: E402
import job_plan  # noqa: E402

ALL_JOBS = sorted(job_plan.GROUP.keys())
_pass = 0
_fail = 0


def plan_for(paths, event="pull_request", force_full=False):
    return classify(list(paths), event=event, force_full=force_full)["plan"]


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
check("docs-only: fuzz-native may skip", "fuzz-native-security" in s)
check("docs-only: lint never skips", "lint" not in s)
check("docs-only: classify never skips", "classify" not in s)

# -- pure JS frontend: focused-js + fuzz-js run; matrix + lua skip --
f = flags(["stdlib/cli/js/hull/source/parser.js"])
check("pure-js: run_focused_js", f["run_focused_js"])
check("pure-js: run_fuzz_js", f["run_fuzz_js"])
check("pure-js: NOT run_full_matrix", not f["run_full_matrix"])
check("pure-js: NOT run_focused_lua", not f["run_focused_lua"])
s = skips(["stdlib/cli/js/hull/source/parser.js"])
check("pure-js: build may skip", "build" in s)
check("pure-js: focused-lua may skip", "focused-lua-frontend" in s)
check("pure-js: fuzz-native may skip", "fuzz-native-security" in s)
check("pure-js: focused-js NOT skippable", "focused-js-frontend" not in s)
check("pure-js: fuzz-js NOT skippable", "fuzz-js-source" not in s)

# -- pure Lua frontend --
f = flags(["stdlib/cli/lua/hull/source/lexer.lua"])
check("pure-lua: run_focused_lua", f["run_focused_lua"])
check("pure-lua: run_fuzz_lua", f["run_fuzz_lua"])
check("pure-lua: NOT run_full_matrix", not f["run_full_matrix"])
s = skips(["stdlib/cli/lua/hull/source/lexer.lua"])
check("pure-lua: focused-lua NOT skippable", "focused-lua-frontend" not in s)
check("pure-lua: build may skip", "build" in s)

# -- fuzz-only: a source-parser fuzzer .c sets frontend+fuzz -> narrow --
f = flags(["fuzz/fuzz_js_source.c"])
check("js-fuzz .c: run_fuzz_js + focused_js", f["run_fuzz_js"] and f["run_focused_js"])
check("js-fuzz .c: NOT run_full_matrix", not f["run_full_matrix"])
# a NATIVE fuzzer .c is NOT a proven narrow class -> broad
f = flags(["fuzz/fuzz_pgwire.c"])
check("native-fuzz .c -> broad (run_full_matrix)", f["run_full_matrix"])

# -- core: broad, everything runs --
f = flags(["src/hull/cap/db.c"])
check("core: run_full_matrix", f["run_full_matrix"])
check("core: all group flags true", all(f.values()))
check("core: nothing skippable except benchmark(non-push)",
      skips(["src/hull/cap/db.c"]) == {"benchmark"})

# -- mixed core + frontend: full core PLUS focused (all run) --
f = flags(["stdlib/cli/js/hull/source/parser.js", "src/hull/cap/db.c"])
check("mixed: run_full_matrix", f["run_full_matrix"])
check("mixed: run_focused_js", f["run_focused_js"])
check("mixed: build NOT skippable", "build" not in skips(["stdlib/cli/js/hull/source/parser.js", "src/hull/cap/db.c"]))

# -- main push / force-full: everything runs regardless of paths --
f = flags(["docs/x.md"], event="push_main")
check("push_main: run_full_matrix", f["run_full_matrix"] and all(f.values()))
check("push_main: only benchmark not force-skippable (event=push => none)",
      job_plan.allow_skip_jobs(plan_for(["docs/x.md"], event="push_main"), ALL_JOBS, "push") == set())
f = flags(["docs/x.md"], force_full=True)
check("force-full: run_full_matrix", f["run_full_matrix"] and all(f.values()))

# -- unexpected skip: build applicable in a broad plan -> NOT in allow-skip --
check("broad: build must run (not in allow-skip)",
      "build" not in job_plan.allow_skip_jobs(plan_for(["src/hull/cap/db.c"]), ALL_JOBS, "pull_request"))

# -- missing applicability: an UNMAPPED job defaults to always -> never skips --
s = job_plan.allow_skip_jobs(plan_for(["docs/x.md"]), ALL_JOBS + ["brand_new_job"], "pull_request")
check("unmapped job defaults applicable (never skippable)", "brand_new_job" not in s)
check("but a mapped full-matrix job IS skippable in docs-only", "build" in s)

# -- benchmark: push-only; skippable on PR regardless of plan, NOT on push --
check("benchmark skippable on PR (broad plan)",
      "benchmark" in job_plan.allow_skip_jobs(plan_for(["src/hull/cap/db.c"]), ALL_JOBS, "pull_request"))
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
check("existing non-narrow flag alone (focused_wasm) -> broad", is_broad({"lint": True, "focused_wasm": True}))
check("lint-only (no narrow selector) -> broad", is_broad({"lint": True}))
check("full_all present -> broad", is_broad({"lint": True, "full_all": True}))
# a genuinely-approved narrow plan still narrows (positive path still works)
check("approved narrow (docs_only) -> not broad", not is_broad({"lint": True, "docs_only": True}))
check("approved narrow (js) -> focused-js only",
      job_plan.run_flags({"lint": True, "focused_js_frontend": True})["run_focused_js"]
      and not job_plan.run_flags({"lint": True, "focused_js_frontend": True})["run_full_matrix"])

print("job_plan fixtures: %d passed, %d failed" % (_pass, _fail))
sys.exit(1 if _fail else 0)
