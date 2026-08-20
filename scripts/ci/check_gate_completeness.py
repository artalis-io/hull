#!/usr/bin/env python3
# check_gate_completeness.py - assert the ci-success gate depends on EVERY ci.yml
# job (docs/ci_architecture_design.md section 16: "a required job with skipped,
# MISSING, failure, or cancellation fails"). A job absent from the gate's `needs`
# is invisible to the gate, so its failure could pass unnoticed. This guard runs
# in the `classify` job and fails CI if a job was added without wiring it into
# the gate. No PyYAML dependency (parses the fixed 2-space job layout + the
# ci-success block-list `needs`).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import re
import sys

WORKFLOW = ".github/workflows/ci.yml"
GATE = "ci-success"


def parse(path):
    lines = open(path, encoding="utf-8").read().splitlines()
    # locate the top-level `jobs:` block
    n = len(lines)
    i = 0
    while i < n and not re.match(r"^jobs:\s*$", lines[i]):
        i += 1
    i += 1
    job_ids = []
    gate_needs = None
    while i < n:
        ln = lines[i]
        if re.match(r"^\S", ln):        # dedented out of jobs:
            break
        m = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", ln)
        if m:
            jid = m.group(1)
            job_ids.append(jid)
            if jid == GATE:
                gate_needs = parse_needs(lines, i + 1, n)
        i += 1
    return job_ids, gate_needs


def parse_needs(lines, start, n):
    """Parse the `needs:` of the job whose body begins at `start`. Supports a
    block list (`needs:\\n      - a\\n      - b`) and a flow list on one line
    (`needs: [a, b, c]`). Returns a set of job ids, or None if no needs found
    before the next job."""
    i = start
    while i < n:
        ln = lines[i]
        if re.match(r"^  [A-Za-z0-9_-]+:\s*$", ln):   # next job key -> stop
            return None
        m_flow = re.match(r"^    needs:\s*\[(.*)\]\s*$", ln)
        if m_flow:
            return {s.strip() for s in m_flow.group(1).split(",") if s.strip()}
        if re.match(r"^    needs:\s*$", ln):
            got = set()
            j = i + 1
            while j < n:
                mi = re.match(r"^      -\s*([A-Za-z0-9_-]+)\s*$", lines[j])
                if mi:
                    got.add(mi.group(1)); j += 1; continue
                break
            return got
        i += 1
    return None


def main():
    job_ids, gate_needs = parse(WORKFLOW)
    if GATE not in job_ids:
        print("check-gate: no %r job found in %s" % (GATE, WORKFLOW)); return 1
    if gate_needs is None:
        print("check-gate: %r has no parseable `needs:`" % GATE); return 1

    expected = set(job_ids) - {GATE}
    missing = expected - gate_needs               # a job not gated
    extra = gate_needs - set(job_ids)             # a needs entry that is not a real job
    problems = []
    if missing:
        problems.append("jobs NOT in %s.needs (ungated): %s" % (GATE, sorted(missing)))
    if extra:
        problems.append("%s.needs references unknown jobs: %s" % (GATE, sorted(extra)))
    print("check-gate: %d jobs, %d gated." % (len(job_ids), len(gate_needs)))
    if problems:
        for p in problems:
            print("  FAIL:", p)
        return 1
    print("check-gate: ci-success depends on every job. OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
