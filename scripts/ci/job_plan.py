#!/usr/bin/env python3
# job_plan.py - the SINGLE job-applicability map (docs/ci_architecture_design.md
# section 16). Slice 3b. Both the ci.yml job `if:` conditions AND the ci-success
# gate's allow-skip derive from THIS map, so they can never drift apart.
#
# Model (per the ratified 3b constraints):
#   - Every job DEFAULTS to applicable; only an EXPLICITLY mapped job may skip.
#     An unmapped job falls in the `always` group and must always run + succeed.
#   - Only PROVEN narrow classes skip today: docs-only, JS frontend/fuzz, Lua
#     frontend/fuzz. Any other plan (focused_tooling, project-discovery, query,
#     compute, db, gpu, tls, examples, generic tests, unknown, full_core,
#     full_all) triggers the BROAD suite.
#   - main pushes / full_all / force-full run everything.
#   - Mixed core+frontend runs full core PLUS the relevant focused jobs.
#
# A job belongs to exactly one GROUP. The classifier PLAN maps to a set of
# APPLICABLE groups; a job whose group is not applicable may skip. `benchmark`
# is additionally push-only, so it may skip on any non-push event regardless of
# plan (matches its `if:` in ci.yml).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import classify_changes as _classify  # noqa: E402  (the canonical plan schema)

# Each GROUP has a run-flag emitted by the classify job; the ci.yml job `if:`
# references exactly this flag. `always` has no flag (never skips).
GROUP_FLAG = {
    "full-matrix": "run_full_matrix",
    "focused-js": "run_focused_js",
    "focused-lua": "run_focused_lua",
    "fuzz-js": "run_fuzz_js",
    "fuzz-lua": "run_fuzz_lua",
    "fuzz-native": "run_fuzz_native",
}

# job -> group. Explicit for every gated job. `check_job_plan_consistency.py`
# asserts this matches the ci.yml `if:` flags AND covers every ci.yml job.
GROUP = {
    # always-on (never skip): the orchestrator, lint, and the gate itself.
    "classify": "always", "lint": "always", "ci-success": "always",
    # focused source-frontend + parser fuzz (the proven narrow classes).
    "focused-js-frontend": "focused-js",
    "focused-lua-frontend": "focused-lua",
    "fuzz-js-source": "fuzz-js",
    "fuzz-lua-source": "fuzz-lua",
    # Slice 4 checkpoint 2: the single fuzz-native-security job is SPLIT into three
    # atomic jobs (core / db-wire / compute). In checkpoint 2 they stay in the
    # SAME `fuzz-native` group and gate on the SAME run_fuzz_native flag, so they
    # run/skip exactly as the one job did (no new skipping). Checkpoint 3 reassigns
    # them to their final groups (core-common / db-any / compute).
    "fuzz-core-security": "fuzz-native",
    "fuzz-db-wire": "fuzz-native",
    "fuzz-compute-span": "fuzz-native",
    # everything else = the broad matrix.
    "build": "full-matrix",
    "wasm-readonly-heap-aot": "full-matrix",
    "mapped-span-bench": "full-matrix",
    "compute-aot-shared-heap": "full-matrix",
    "compute-memops-freestanding": "full-matrix",
    "stream-meta": "full-matrix",
    "spans-example": "full-matrix",
    "spans-multi": "full-matrix",
    "spans-hugefile": "full-matrix",
    "wasm-guarded-aot-arm64": "full-matrix",
    "flavors": "full-matrix",
    "reproducibility": "full-matrix",
    "reproducibility-container": "full-matrix",
    "reproducibility-container-interleave": "full-matrix",
    "reproducibility-cosmo": "full-matrix",
    "reproducibility-cosmo-compare": "full-matrix",
    "build-pipeline": "full-matrix",
    "sanitizers": "full-matrix",
    "msan": "full-matrix",
    "tsan": "full-matrix",
    "tsan-shared-heap": "full-matrix",
    "postgres": "full-matrix",
    "mysql": "full-matrix",
    "valkey": "full-matrix",
    "analyze": "full-matrix",
    "cosmo": "full-matrix",
    "gpu": "full-matrix",
    "duckdb": "full-matrix",
    "duckdb-feature": "full-matrix",
    "gpu-feature": "full-matrix",
    "tui-feature": "full-matrix",
    "postgres-feature": "full-matrix",
    "mysql-feature": "full-matrix",
    "project-discovery-lua": "full-matrix",
    "coverage": "full-matrix",
    "htmx-browser": "full-matrix",
    "embed-rust": "full-matrix",
    "embed-zig": "full-matrix",
    "musl": "full-matrix",
    "benchmark": "full-matrix",
}

# POSITIVE narrow allowlist (fail closed). A plan is NARROW - and may skip the
# broad matrix - ONLY when it is a well-formed plan dict whose TRUE flags ALL lie
# within APPROVED_NARROW and include at least one real narrow SELECTOR (beyond the
# always-on lint). An empty plan, a non-dict, a non-boolean field, an unknown
# field, a true flag outside the approved set (a newly added plan flag, or
# focused_wasm, ...), or lint-only -> BROAD. This is a positive allowlist, NOT
# "absence of known broad flags", so a new/unknown/malformed flag can NEVER open
# a skip.
APPROVED_NARROW = frozenset({
    "lint", "docs_only",
    "focused_js_frontend", "focused_js_fuzz",
    "focused_lua_frontend", "focused_lua_fuzz",
})
NARROW_SELECTORS = APPROVED_NARROW - {"lint"}      # at least one of these must be set
KNOWN_PLAN_KEYS = frozenset(_classify.PLAN)        # the canonical plan schema (classify_changes)


def _is_narrow(plan):
    """True iff `plan` is a well-formed plan dict that positively qualifies to
    skip the broad matrix (APPROVED_NARROW). Anything else -> False -> broad."""
    if not isinstance(plan, dict):
        return False
    for k, v in plan.items():
        if not isinstance(v, bool):
            return False                            # non-boolean field -> broad
        if k not in KNOWN_PLAN_KEYS:
            return False                            # unknown field -> broad
    true_flags = {k for k, v in plan.items() if v}
    if not true_flags <= APPROVED_NARROW:
        return False                                # a true flag outside the approved set -> broad
    if not (true_flags & NARROW_SELECTORS):
        return False                                # lint-only / no real narrow selector -> broad
    return True


def applicable_groups(plan):
    """The set of groups that MUST run. `always` is always applicable. A plan is
    only NARROW under the positive allowlist; anything else is BROAD (every group
    applicable). A narrow plan enables just the matching focused + parser-fuzz
    groups."""
    groups = {"always"}
    if not _is_narrow(plan):
        groups |= set(GROUP_FLAG.keys())
        return groups
    if plan.get("focused_js_frontend") or plan.get("focused_js_fuzz"):
        groups |= {"focused-js", "fuzz-js"}
    if plan.get("focused_lua_frontend") or plan.get("focused_lua_fuzz"):
        groups |= {"focused-lua", "fuzz-lua"}
    return groups


# -- Slice 4 native-subsystem group scaffolding (Appendix C.2/C.5) ------------
# The INTENDED checkpoint-3 mapping from a plan to native subsystem groups.
# core-common is the always-run floor for ANY production-C / native change
# (constraint 1). db-any is the SINGLE umbrella group whose sole member is the
# fuzz-db-wire job = the union of the db sub-groups (Amendment 1), keyed off
# focused_db (set whenever any db file changed, backend or shared). This is INERT
# in checkpoint 2: proven by fixtures (test_job_plan.py :: native_groups.*) but NOT
# consulted by applicable_groups - a native plan is not APPROVED_NARROW, so it
# still evaluates BROAD and NOTHING skips. Checkpoint 3 wires this in AND regroups
# the current `full-matrix` jobs into `core-common` + these subsystem groups.
NATIVE_SUBSYSTEM_GROUPS = frozenset({
    "core-common", "db-postgres", "db-mysql", "db-valkey", "db-duckdb", "db-any",
    "gpu", "compute",
})


def _well_formed_plan(plan):
    """A well-formed plan is a dict whose values are ALL bool, whose keys are ALL
    known, and that carries the always-on `lint`. An empty `{}`, a list, an unknown
    key, or a non-bool value is malformed (-> fail closed to broad)."""
    if not isinstance(plan, dict):
        return False
    for k, v in plan.items():
        if not isinstance(v, bool) or k not in KNOWN_PLAN_KEYS:
            return False
    return bool(plan.get("lint"))


def _is_broad_plan(plan):
    """A plan that runs the whole suite: a shared/core/composition/main/force-full
    change (full_all / full_core), OR anything malformed (fail closed). Native
    narrowing only applies to a well-formed, non-broad, native-fact plan."""
    if not _well_formed_plan(plan):
        return True
    return bool(plan.get("full_all") or plan.get("full_core"))


def native_groups(plan):
    """The native subsystem groups a plan selects UNDER the checkpoint-3 mapping.
    Broad/malformed -> every native group. A narrow-native plan -> the always-run
    `core-common` floor plus the specific subsystem group(s), and the `db-any`
    umbrella iff any db backend is touched (via focused_db). Inert scaffolding in
    checkpoint 2 (fixture-proven, not yet consulted by applicable_groups)."""
    if _is_broad_plan(plan):
        return set(NATIVE_SUBSYSTEM_GROUPS)
    g = set()
    if plan.get("focused_db_postgres"):
        g.add("db-postgres")
    if plan.get("focused_db_mysql"):
        g.add("db-mysql")
    if plan.get("focused_db_valkey"):
        g.add("db-valkey")
    if plan.get("focused_db_duckdb"):
        g.add("db-duckdb")
    if plan.get("focused_db"):
        g.add("db-any")                        # the fuzz-db-wire umbrella
    if plan.get("focused_gpu"):
        g.add("gpu")
    if plan.get("focused_compute") or plan.get("focused_wasm"):
        g.add("compute")
    if g:
        g.add("core-common")                   # the always-run floor for any native change
    return g


def run_flags(plan):
    """The classify-job outputs the ci.yml job `if:` conditions key off."""
    g = applicable_groups(plan)
    return {flag: (grp in g) for grp, flag in GROUP_FLAG.items()}


def allow_skip_jobs(plan, all_jobs, event):
    """The jobs the gate PERMITS to skip. A job may skip iff its group is not
    applicable AND its group is not `always`. An unmapped job -> `always` ->
    never skippable (default applicable). `benchmark` is push-only: it may also
    skip on any non-push event regardless of plan."""
    g = applicable_groups(plan)
    skip = set()
    for j in all_jobs:
        grp = GROUP.get(j, "always")
        if grp != "always" and grp not in g:
            skip.add(j)
    if event != "push" and "benchmark" in all_jobs:
        skip.add("benchmark")
    return skip


def _emit_github_output(flags):
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as f:
        for k, v in sorted(flags.items()):
            f.write("%s=%s\n" % (k, "true" if v else "false"))


def main(argv):
    ap = argparse.ArgumentParser(description="Emit per-group run flags from a classifier plan.")
    ap.add_argument("--plan", required=True, help="the plan JSON (from classify_changes.py plan_json).")
    args = ap.parse_args(argv)
    try:
        plan = json.loads(args.plan)
    except Exception:
        # A malformed plan must fail CLOSED to broad (run everything).
        plan = {"full_all": True}
    flags = run_flags(plan)
    print(json.dumps(flags, sort_keys=True, indent=2))
    _emit_github_output(flags)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
