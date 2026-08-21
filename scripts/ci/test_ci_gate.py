#!/usr/bin/env python3
# test_ci_gate.py - fixtures for the CI result gate (docs/ci_architecture_design.md
# sections 16 + 19). Pure Python, no GitHub Actions. Proves the gate on success,
# failure, cancellation, and legitimate/illegitimate skips + fail-closed states.
# Run: python3 scripts/ci/test_ci_gate.py
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ci_gate import evaluate  # noqa: E402

_pass = 0
_fail = 0


def R(**kw):
    """Build a needs-like map from job=result kwargs."""
    return {k: {"result": v} for k, v in kw.items()}


def expect(name, needs, allow_skip=(), require_present=("classify",), should_pass=True):
    global _pass, _fail
    problems = evaluate(needs, allow_skip=allow_skip, require_present=require_present)
    passed = (len(problems) == 0)
    if passed == should_pass:
        _pass += 1
    else:
        _fail += 1
        print("FAIL: %s :: expected %s, got problems=%s"
              % (name, "PASS" if should_pass else "FAIL", problems))


# all success -> pass
expect("all success", R(classify="success", build="success", lint="success"),
       should_pass=True)

# a single failure -> fail
expect("one failure", R(classify="success", build="failure", lint="success"),
       should_pass=False)

# a cancellation -> fail
expect("one cancelled", R(classify="success", build="cancelled", lint="success"),
       should_pass=False)

# a required job skipped (not allowed) -> fail
expect("required job skipped", R(classify="success", build="skipped", lint="success"),
       should_pass=False)

# a legitimately inapplicable job skipped (allow_skip) -> pass
expect("legit skip (benchmark on PR)",
       R(classify="success", build="success", benchmark="skipped", lint="success"),
       allow_skip=("benchmark",), should_pass=True)

# benchmark is push-only: on a PUSH plan it IS applicable (allow_skip empty), so a
# skipped benchmark must FAIL - a mistakenly skipped benchmark on main must not pass.
expect("benchmark skipped on push plan -> fail",
       R(classify="success", build="success", benchmark="skipped", lint="success"),
       allow_skip=(), should_pass=False)

# a job in allow_skip that actually ran and succeeded -> still pass
expect("allow_skip job succeeded (push)",
       R(classify="success", build="success", benchmark="success"),
       allow_skip=("benchmark",), should_pass=True)

# an allow_skip job that FAILED is still a failure (allow_skip only excuses skip)
expect("allow_skip job failed -> fail",
       R(classify="success", build="success", benchmark="failure"),
       allow_skip=("benchmark",), should_pass=False)

# classify missing entirely -> fail closed (require_present)
expect("classify missing", R(build="success", lint="success"), should_pass=False)

# classify present but not success -> fail
expect("classify failed", R(classify="failure", build="success"), should_pass=False)
expect("classify skipped", R(classify="skipped", build="success"),
       allow_skip=("classify",), should_pass=False)   # even if allowed to skip, require_present forbids

# unknown / null result -> fail closed
expect("null result", R(classify="success", build="success", weird=None), should_pass=False)
expect("unknown string result",
       {"classify": {"result": "success"}, "build": {"result": "neutral"}},
       should_pass=False)
expect("missing result key",
       {"classify": {"result": "success"}, "build": {}}, should_pass=False)

# empty / non-dict needs -> fail closed
expect("empty needs", {}, should_pass=False)
expect("non-dict needs", [], should_pass=False)   # type: ignore

# matrix aggregate: a matrix job exposes one aggregate result; a failed leg -> failure
expect("matrix aggregate failure",
       R(classify="success", build="failure"), should_pass=False)
expect("matrix aggregate success",
       R(classify="success", build="success"), should_pass=True)

# a realistic PR shape: all real jobs success, benchmark skipped -> pass
expect("realistic PR (benchmark skipped, rest success)",
       R(classify="success", build="success", sanitizers="success", fuzz="success",
         cosmo="success", lint="success", benchmark="skipped"),
       allow_skip=("benchmark",), should_pass=True)

# -- Slice 5A: the wamrc producer/verify are compute-grouped. On a compute-
#    applicable plan they are NOT allowed skips, so a producer failure (which
#    skips its consumer via `needs`) and a consumer verification failure BOTH
#    reach a RED gate; on a non-compute plan they legitimately skip. The allow-skip
#    set is DERIVED from the shared map (not a hand list), so the gate and the job
#    `if:` conditions cannot disagree.
import job_plan  # noqa: E402
from classify_changes import classify  # noqa: E402

_ALL5 = sorted(job_plan.GROUP.keys())
_compute_plan = classify(["src/hull/cap/wasm.c"])["plan"]   # narrow compute -> compute applicable
_allow5 = job_plan.allow_skip_jobs(_compute_plan, _ALL5, "pull_request")
_docs_plan = classify(["docs/x.md"])["plan"]
_allow_docs = job_plan.allow_skip_jobs(_docs_plan, _ALL5, "pull_request")


def _ck(name, cond):
    global _pass, _fail
    if cond:
        _pass += 1
    else:
        _fail += 1
        print("FAIL:", name)


_ck("5A: producer NOT an allowed skip on a compute plan", "wamrc-x86_64" not in _allow5)
_ck("5A: verify NOT an allowed skip on a compute plan", "wamrc-artifact-verify" not in _allow5)
_ck("5A: producer IS an allowed skip on a docs plan", "wamrc-x86_64" in _allow_docs)
_ck("5A: verify IS an allowed skip on a docs plan", "wamrc-artifact-verify" in _allow_docs)

# producer failure -> its consumer is skipped by `needs`; on a compute plan that
# applicable skip is DISALLOWED -> gate FAIL (the producer failure also fails it).
expect("5A: producer failure + consumer skipped -> gate FAIL",
       R(classify="success", build="success",
         **{"wamrc-x86_64": "failure", "wamrc-artifact-verify": "skipped"}),
       allow_skip=_allow5, should_pass=False)
# a consumer verification failure -> gate FAIL (never silently rebuilt).
expect("5A: consumer verify failure -> gate FAIL",
       R(classify="success", build="success",
         **{"wamrc-x86_64": "success", "wamrc-artifact-verify": "failure"}),
       allow_skip=_allow5, should_pass=False)
# the missing-artifact case surfaces as the consumer FAILING (not skipping) -> FAIL.
expect("5A: missing-artifact consumer failure -> gate FAIL",
       R(classify="success", build="success",
         **{"wamrc-x86_64": "success", "wamrc-artifact-verify": "failure"}),
       allow_skip=_allow5, should_pass=False)
# happy path -> pass.
expect("5A: producer + verify success -> gate pass",
       R(classify="success", build="success",
         **{"wamrc-x86_64": "success", "wamrc-artifact-verify": "success"}),
       allow_skip=_allow5, should_pass=True)
# on a non-compute (docs) plan they legitimately skip -> pass.
expect("5A: producer+verify skipped on docs plan -> gate pass",
       R(classify="success", lint="success",
         **{"wamrc-x86_64": "skipped", "wamrc-artifact-verify": "skipped"}),
       allow_skip=_allow_docs, should_pass=True)

print("ci_gate fixtures: %d passed, %d failed" % (_pass, _fail))
sys.exit(1 if _fail else 0)
