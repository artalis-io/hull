#!/usr/bin/env python3
# ci_gate.py - the applicability-aware CI result gate (docs/ci_architecture_design.md
# section 16). Slice 2. Consumes the GitHub `needs` context (toJSON(needs)) and
# FAILS unless every required job succeeded.
#
# `if: always()` on the gate job only makes it EVALUATE; this script decides
# pass/fail. It must NOT merely accept every `skipped` result:
#   - success                         -> ok
#   - skipped AND declared inapplicable (allow_skip) -> ok
#   - skipped but NOT inapplicable    -> FAIL (a required job was skipped)
#   - failure / cancelled             -> FAIL
#   - missing / null / unknown result -> FAIL (fail closed)
# A matrix job exposes ONE aggregate result to `needs`, so checking that result
# is correct (a failed/cancelled leg makes the aggregate failure/cancelled).
#
# In Slice 2 no job is classifier-skipped, so the only legitimate skip is a
# genuinely conditional job (e.g. the push-only `benchmark` on a PR), passed via
# --allow-skip. When classifier-based skipping lands, the inapplicable set is
# derived from the plan.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import argparse
import json
import sys


def evaluate(needs, allow_skip=None, require_present=()):
    """Return a list of problems (empty => the gate passes). `needs` is the
    toJSON(needs) map {job: {"result": "...", ...}}. `allow_skip` jobs may be
    skipped; every other job must be success. `require_present` jobs must exist
    AND be success (catches a required job dropped from the gate's needs)."""
    allow_skip = set(allow_skip or ())
    if not isinstance(needs, dict) or not needs:
        return ["no needs results -> fail closed"]
    problems = []
    for job, info in sorted(needs.items()):
        result = (info or {}).get("result")
        if result == "success":
            continue
        if result == "skipped":
            if job not in allow_skip:
                problems.append("%s: required job was SKIPPED" % job)
            continue
        if result in ("failure", "cancelled"):
            problems.append("%s: %s" % (job, result))
            continue
        problems.append("%s: unexpected result %r" % (job, result))   # None / "" / unknown
    for job in require_present:
        info = needs.get(job)
        if not info or info.get("result") != "success":
            problems.append("required job %r missing or not success" % job)
    return problems


def main(argv):
    ap = argparse.ArgumentParser(description="Applicability-aware CI result gate.")
    ap.add_argument("--needs", required=True, help="path to a file holding toJSON(needs).")
    ap.add_argument("--allow-skip", default="", help="comma-separated jobs that may be skipped.")
    ap.add_argument("--require-present", default="classify",
                    help="comma-separated jobs that must be present AND success.")
    args = ap.parse_args(argv)

    try:
        with open(args.needs, "r", encoding="utf-8") as f:
            needs = json.load(f)
    except Exception as e:   # unreadable / malformed needs -> fail closed.
        print("ci-gate: cannot read needs (%s) -> FAIL closed" % e)
        return 1

    allow = [s for s in args.allow_skip.split(",") if s]
    reqp = [s for s in args.require_present.split(",") if s]
    problems = evaluate(needs, allow_skip=allow, require_present=reqp)

    total = len(needs) if isinstance(needs, dict) else 0
    ok = sum(1 for v in (needs.values() if isinstance(needs, dict) else [])
             if (v or {}).get("result") == "success")
    print("ci-gate: %d/%d jobs succeeded; allow_skip=%s" % (ok, total, allow))
    if problems:
        for p in problems:
            print("  FAIL:", p)
        print("ci-gate: FAILED (%d problem(s))." % len(problems))
        return 1
    print("ci-gate: all required jobs passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
