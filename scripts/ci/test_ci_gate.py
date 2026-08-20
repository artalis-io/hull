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

print("ci_gate fixtures: %d passed, %d failed" % (_pass, _fail))
sys.exit(1 if _fail else 0)
