#!/usr/bin/env python3
# job_plan.py - the SINGLE job-applicability map (docs/ci_architecture_design.md
# section 16). Both the ci.yml job `if:` conditions AND the ci-success
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
#
# the monolithic `full-matrix` + `fuzz-native` groups are
# split into the always-run `core-common` floor + the native subsystem groups
# (db-* / db-any umbrella / gpu / compute) + the broad-only `web` bucket
# (htmx-browser, project-discovery-lua) that a native change does not touch. The
# frontend groups (focused/fuzz js/lua) are unchanged.
GROUP_FLAG = {
    "core-common": "run_core_common",
    "web": "run_web",
    "db-postgres": "run_db_postgres",
    "db-mysql": "run_db_mysql",
    "db-valkey": "run_db_valkey",
    "db-duckdb": "run_db_duckdb",
    "db-any": "run_db_any",
    "gpu": "run_gpu",
    "compute": "run_compute",
    "focused-js": "run_focused_js",
    "focused-lua": "run_focused_lua",
    "fuzz-js": "run_fuzz_js",
    "fuzz-lua": "run_fuzz_lua",
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
    # -- core-common: the subsystem-AGNOSTIC floor. Runs for EVERY production-C /
    # native change AND every broad run (constraint 1). Includes the four-platform
    # `make test` (all unit binaries), build/link/repro/platform checks, the
    # sanitizer + static-analysis floor, and the core security fuzzers.
    # fuzz-core-security is assigned here (was the shared fuzz-native).
    "build": "core-common",
    "build-pipeline": "core-common",
    "flavors": "core-common",
    "sanitizers": "core-common",
    "msan": "core-common",
    "tsan": "core-common",
    "analyze": "core-common",
    "coverage": "core-common",
    "reproducibility": "core-common",
    "reproducibility-container": "core-common",
    "reproducibility-container-interleave": "core-common",
    "reproducibility-cosmo": "core-common",
    "reproducibility-cosmo-compare": "core-common",
    "cosmo": "core-common",
    "musl": "core-common",
    "embed-rust": "core-common",
    "embed-zig": "core-common",
    "tui-feature": "core-common",
    "fuzz-core-security": "core-common",
    "benchmark": "core-common",          # push-only (extra event guard in its if:)
    # -- db subsystem (per-backend integrations) --
    "postgres": "db-postgres",
    "postgres-feature": "db-postgres",
    "pg-transport-sanitize": "db-postgres",
    "mysql": "db-mysql",
    "mysql-feature": "db-mysql",
    "valkey": "db-valkey",
    "duckdb": "db-duckdb",
    "duckdb-feature": "db-duckdb",
    # the fuzz-db-wire umbrella (Amendment 1): one atomic job, group db-any =
    # applicable iff ANY db backend is touched (or broad).
    "fuzz-db-wire": "db-any",
    # -- gpu subsystem --
    "gpu": "gpu",
    "gpu-feature": "gpu",
    # -- compute subsystem (AOT cluster + shared-heap TSan + span fuzzer +
    # the Slice-5A wamrc producer/verify) --
    "wamrc-x86_64": "compute",
    "wamrc-artifact-verify": "compute",
    "wasm-readonly-heap-aot": "compute",
    "mapped-span-bench": "compute",
    "compute-aot-shared-heap": "compute",
    "compute-memops-freestanding": "compute",
    "stream-meta": "compute",
    "spans-example": "compute",
    "spans-multi": "compute",
    "spans-hugefile": "compute",
    "wasm-guarded-aot-arm64": "compute",
    "tsan-shared-heap": "compute",
    "fuzz-compute-span": "compute",
    # -- web/integration (broad-only): a native (db/gpu/compute) change does not
    # touch these, so a narrow-native plan skips them; a broad plan runs them. --
    "htmx-browser": "web",
    "project-discovery-lua": "web",
}

# POSITIVE narrow allowlist (fail closed). A plan may skip the broad matrix ONLY
# when it is a well-formed plan dict whose TRUE flags ALL lie within
# APPROVED_NARROW and include at least one real narrow SELECTOR (beyond the
# always-on lint). EXTENDS the allowlist from the frontend
# classes to the native subsystem selectors. Its OWN positive set is the union of:
#   - {lint, docs_only}
#   - FRONTEND_SELECTORS  (the proven source-frontend/parser classes)
#   - NATIVE_SELECTORS    (the DB backend / gpu / compute classes)
# A native change MIXED with any NON-approved true flag (focused_tooling,
# focused_project_discovery, focused_query, focused_tls, focused_native_fuzz,
# tests/examples -> full_core, or ANY future/unknown/non-boolean field) falls
# outside the set -> BROAD (constraint: fail closed). This is a positive
# allowlist, NOT "absence of known broad flags", so a new/unknown/malformed flag
# can NEVER open a skip.
FRONTEND_SELECTORS = frozenset({
    "focused_js_frontend", "focused_js_fuzz",
    "focused_lua_frontend", "focused_lua_fuzz",
})
NATIVE_SELECTORS = frozenset({
    "focused_db", "focused_db_postgres", "focused_db_mysql", "focused_db_valkey",
    "focused_db_duckdb", "focused_gpu", "focused_compute", "focused_wasm",
})
APPROVED_NARROW = frozenset({"lint", "docs_only"}) | FRONTEND_SELECTORS | NATIVE_SELECTORS
NARROW_SELECTORS = APPROVED_NARROW - {"lint"}      # at least one of these must be set
KNOWN_PLAN_KEYS = frozenset(_classify.PLAN)        # the canonical plan schema (classify_changes)


def _is_narrow(plan):
    """True iff `plan` is a well-formed plan dict that positively qualifies to
    skip the broad matrix (APPROVED_NARROW). Anything else -> False -> broad."""
    if not _well_formed_plan(plan):
        return False                                # malformed / empty / lint-missing -> broad
    if plan.get("full_all") or plan.get("full_core"):
        return False                                # a broad plan -> broad
    true_flags = {k for k, v in plan.items() if v}
    if not true_flags <= APPROVED_NARROW:
        return False                                # a true flag outside the approved set -> broad
    if not (true_flags & NARROW_SELECTORS):
        return False                                # lint-only / no real narrow selector -> broad
    return True


def applicable_groups(plan):
    """The set of groups that MUST run. `always` is always applicable. A plan is
    only NARROW under the positive allowlist; anything else is BROAD (every group
    applicable). A narrow plan is ADDITIVE: the matching source-frontend + parser
    -fuzz groups (from FRONTEND_SELECTORS) PLUS the native subsystem groups (from
    native_groups(): the core-common floor + the touched db/gpu/compute groups +
    the db-any umbrella). A native+frontend change therefore runs core-common +
    its subsystem + the frontend/fuzzer groups; the broad-only `web` group is
    never added by a narrow plan (a native/frontend change does not touch it)."""
    groups = {"always"}
    if not _is_narrow(plan):
        groups |= set(GROUP_FLAG.keys())            # broad: every group applicable
        return groups
    if plan.get("focused_js_frontend") or plan.get("focused_js_fuzz"):
        groups |= {"focused-js", "fuzz-js"}
    if plan.get("focused_lua_frontend") or plan.get("focused_lua_fuzz"):
        groups |= {"focused-lua", "fuzz-lua"}
    groups |= native_groups(plan)                   # {} for a pure frontend/docs plan
    return groups


# -- native-subsystem groups (Appendix C.2/C.5) -----------------------
# The mapping from a plan to native subsystem groups, CONSULTED by
# applicable_groups (skipping is active). core-common is the
# always-run floor for ANY production-C / native change (constraint 1). db-any is
# the SINGLE umbrella group whose sole member is the fuzz-db-wire job = the union
# of the db sub-groups (Amendment 1), keyed off focused_db (set whenever any db
# file changed, backend or shared).
NATIVE_SUBSYSTEM_GROUPS = frozenset({
    "core-common", "db-postgres", "db-mysql", "db-valkey", "db-duckdb", "db-any",
    "gpu", "compute",
})


def _well_formed_plan(plan):
    """A well-formed plan is a coherent CANONICAL plan: the EXACT plan schema (no
    missing, extra, or unknown key), every value boolean, the always-on `lint`
    set, AND the classifier's native-selector coherence invariants. Anything
    else - a partial subset, a syntactically-valid-but-incomplete plan, or an
    incoherent selector combination - is malformed and fails closed to BROAD (it
    must not be allowed to narrow to a partial subsystem set). The coherence
    invariants mirror derive_plan()'s contract:
      - focused_db is set IFF at least one backend selector is set;
      - focused_compute and focused_wasm are always paired (set together)."""
    if not isinstance(plan, dict):
        return False
    if set(plan.keys()) != KNOWN_PLAN_KEYS:
        return False                                # not the exact plan schema
    for v in plan.values():
        if not isinstance(v, bool):
            return False                            # non-boolean field
    if not plan["lint"]:
        return False                                # a real plan always runs lint
    # native-selector coherence (derive_plan contract): a backend selector without
    # focused_db, or focused_db without any backend selector, is incoherent.
    any_backend = (plan["focused_db_postgres"] or plan["focused_db_mysql"]
                   or plan["focused_db_valkey"] or plan["focused_db_duckdb"])
    if plan["focused_db"] != any_backend:
        return False
    # the classifier always pairs focused_compute + focused_wasm.
    if plan["focused_compute"] != plan["focused_wasm"]:
        return False
    return True


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
    umbrella iff any db backend is touched (via focused_db). Returns {} for a plan
    with no native selector (a pure frontend/docs narrow)."""
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
