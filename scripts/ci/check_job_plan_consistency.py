#!/usr/bin/env python3
# check_job_plan_consistency.py - assert the ci.yml job `if:` conditions and the
# ci-success gate's allow-skip derive from the SAME job-applicability map
# (job_plan.py). Slice 3b safety guard: prevents a job's skip-condition from
# drifting away from what the gate believes may skip. Run in the `classify` job.
#
# Checks (all must hold):
#   - every ci.yml job (except the always-on classify/lint/ci-success) is mapped
#     in job_plan.GROUP -> a new job must be DELIBERATELY classified;
#   - a job in a SKIPPABLE group has an `if:` referencing exactly that group's
#     run-flag (needs.classify.outputs.<flag>);
#   - an `always` job has NO run-flag `if:` (it must never skip);
#   - no GROUP entry references a non-existent job.
# No PyYAML dependency (parses the fixed job layout + each job's `if:`).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import job_plan  # noqa: E402

WORKFLOW = ".github/workflows/ci.yml"
ALWAYS = {"classify", "lint", "ci-success"}


def parse_jobs_with_if(path):
    lines = open(path, encoding="utf-8").read().splitlines()
    n = len(lines); i = 0
    while i < n and not re.match(r"^jobs:\s*$", lines[i]):
        i += 1
    i += 1
    jobs = {}          # job_id -> if-text (or None)
    cur = None
    while i < n:
        ln = lines[i]
        if re.match(r"^\S", ln):
            break
        m = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", ln)
        if m:
            cur = m.group(1); jobs[cur] = None
        elif cur is not None:
            mi = re.match(r"^    if:\s*(.*)$", ln)
            if mi:
                jobs[cur] = mi.group(1)
        i += 1
    return jobs


def main():
    jobs = parse_jobs_with_if(WORKFLOW)
    problems = []

    # every job mapped (except always-on, which are allowed implicit)
    for j in jobs:
        if j not in job_plan.GROUP:
            problems.append("job %r is not mapped in job_plan.GROUP" % j)

    # GROUP entries must all be real jobs
    for j in job_plan.GROUP:
        if j not in jobs:
            problems.append("job_plan.GROUP maps unknown job %r" % j)

    # if-condition <-> group flag consistency
    for j, iftext in jobs.items():
        grp = job_plan.GROUP.get(j)
        if grp is None:
            continue
        iftext = iftext or ""
        refs = set(re.findall(r"needs\.classify\.outputs\.(run_[a-z_]+)", iftext))
        if grp == "always" or j in ALWAYS:
            if refs:
                problems.append("always-on job %r must not gate on a run-flag (found %s)" % (j, sorted(refs)))
            continue
        flag = job_plan.GROUP_FLAG.get(grp)
        if flag is None:
            problems.append("group %r for job %r has no run-flag" % (grp, j))
        elif flag not in refs:
            problems.append("job %r (group %s) must gate on %s; if=%r" % (j, grp, flag, iftext))

    print("check-job-plan: %d jobs, %d mapped groups." % (len(jobs), len(job_plan.GROUP)))
    if problems:
        for p in problems:
            print("  FAIL:", p)
        return 1
    print("check-job-plan: job `if:` conditions match the shared applicability map. OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
