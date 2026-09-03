#!/usr/bin/env python3
# classify_changes.py - the repository-owned, change-aware CI classifier.
#
# Design: docs/ci_architecture_design.md (Appendix B). The classifier stage of the ratified
# rollout: this emits overlapping change FACTS and a derived execution PLAN. It
# does NOT yet gate any CI job - the workflow wiring (orchestrator + ci-success
# gate) is a separate stage. Keeping this pure + fixture-tested (test_classify_changes.py)
# lets the rules be reviewed and proven independently of GitHub Actions.
#
# Python 3 (not POSIX sh): POSIX `read` cannot reliably consume NUL-delimited
# paths, and Python gives deterministic JSON + adversarial-path fixtures.
#
# Input: the changed paths for a PR, computed from the REAL merge base with
# rename detection DISABLED so a rename appears as BOTH a deleted old path and an
# added new path - each is classified, and the broader classification wins (a
# rename across a trust boundary can never escape into a narrower plan):
#
#     git diff --name-only -z --no-renames "$(git merge-base origin/main HEAD)"..HEAD | \
#         python3 scripts/ci/classify_changes.py --event pull_request
#
# Fail-closed invariants (docs section 6):
#   - unknown path                      -> production_core_changed (core)
#   - main push / schedule / force-full -> full_all (no path classification)
#   - workflow/classifier/Makefile/mk   -> full_all (self-trust, section 8)
#   - empty / ambiguous PR diff         -> full_all
#   - any parse/read failure            -> full_all
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import argparse
import json
import os
import sys

# The complete overlapping fact set (docs section 5). Every path contributes to
# one or more of these; the plan is derived from their union.
FACTS = [
    "tooling_changed",
    "lua_frontend_changed",
    "js_frontend_changed",
    "project_discovery_changed",
    "domain_changed",
    "query_changed",
    "compute_changed",
    "wasm_changed",
    "gpu_changed",
    "native_db_changed",
    # per-backend DB facts (Appendix C): an isolated backend `.c`
    # narrows to its own backend; a shared-DB `.c` sets ALL FOUR (fan-out).
    "db_postgres_changed",
    "db_mysql_changed",
    "db_valkey_changed",
    "db_duckdb_changed",
    "tls_network_changed",
    "production_core_changed",
    "build_composition_changed",
    "ci_changed",
    "js_fuzz_changed",
    "lua_fuzz_changed",
    "native_fuzz_changed",
    "tests_changed",
    "examples_changed",
    "docs",           # a path that IS ordinary documentation (drives docs_only)
    "unknown",        # a path matching no rule -> fail closed to core
]

# Derived plan flags (docs section 5 + Appendix B).
PLAN = [
    "full_all",
    "full_core",
    "focused_js_frontend",
    "focused_lua_frontend",
    "focused_project_discovery",
    "focused_tooling",
    "focused_query",
    "focused_compute",
    "focused_db",
    "focused_db_postgres",
    "focused_db_mysql",
    "focused_db_valkey",
    "focused_db_duckdb",
    "focused_wasm",
    "focused_gpu",
    "focused_tls",
    "focused_js_fuzz",
    "focused_lua_fuzz",
    "focused_native_fuzz",
    "docs_only",
    "lint",
]


# -- isolated-subsystem allowlist (Appendix C.2/C.3) -----------------
# A CURATED, EXACT-PATH positive allowlist of production `.c` files whose
# compile/link/dependency closure is confined to ONE native subsystem. A matched
# file emits ONLY its subsystem fact(s), NOT production_core; every OTHER src/**
# path stays production_core (broad, fail closed). The allowlist is layered OVER
# the broad default, so a renamed/deleted entry loses its narrow route and reverts
# to broad - it can never mis-narrow. Every path here is machine-verified to exist
# (test_classify_changes.py :: allowlist_files_exist).
#
# IMPORTANT (additive/no-skip): none of the focused_* flags these
# facts drive are in job_plan.APPROVED_NARROW, so a plan carrying them still
# evaluates BROAD - the classifier narrows the FACTS but NOTHING skips yet. Skip
# activation (adding the native narrow class + regrouping the matrix) is a
# later step.
DB_POSTGRES_FILES = frozenset({
    "src/hull/cap/db_postgres.c", "src/hull/cap/pgwire.c", "src/hull/cap/pg_conn.c",
    "src/hull/cap/pg_transport.c",
})
DB_MYSQL_FILES = frozenset({
    "src/hull/cap/db_mysql.c", "src/hull/cap/mysqlwire.c", "src/hull/cap/mysql_conn.c",
})
DB_VALKEY_FILES = frozenset({
    "src/hull/cap/valkey.c", "src/hull/cap/valkey_conn.c",
    "src/hull/cap/valkey_register.c", "src/hull/cap/respwire.c",
})
DB_DUCKDB_FILES = frozenset({"src/hull/cap/db_duckdb.c"})
# Shared DB core: on EVERY backend's link path (the db_select.c BACKENDS[] registry
# + the shared cap/registry/dynamic/udf/kv files). A change here fans out to ALL
# backends (constraint 3).
DB_SHARED_FILES = frozenset({
    "src/hull/cap/db.c", "src/hull/cap/db_common.c", "src/hull/cap/db_registry.c",
    "src/hull/cap/db_select.c", "src/hull/cap/db_dynamic.c", "src/hull/cap/db_udf.c",
    "src/hull/cap/kv.c", "src/hull/cap/kv_dynamic.c", "src/hull/cap/kv_feature.c",
})
GPU_FILES = frozenset({
    "src/hull/cap/gpu_wgpu.c", "src/hull/cap/gpu_feature.c", "src/hull/cap/gpu.c",
})
COMPUTE_FILES = frozenset({
    "src/hull/cap/wasm.c", "src/hull/cap/wasm_buffer.c", "src/hull/cap/wasm_data.c",
    "src/hull/cap/wasm_spans.c", "src/hull/cap/wasm_stream.c", "src/hull/worker_wasm.c",
    "src/hull/runtime/lua/mod_compute.c", "src/hull/runtime/js/mod_compute.c",
})
_ALL_DB_BACKEND_FACTS = frozenset({
    "db_postgres_changed", "db_mysql_changed", "db_valkey_changed", "db_duckdb_changed",
})


def _has_component(path, name):
    return name in path.split("/")


def classify_path(path):
    """Return the set of FACTS a single path contributes. FIRST matching rule
    wins so a path maps to exactly one primary class; the union across paths is
    what produces overlap. Ordered most-trusted (widest) first."""
    p = path

    # -- self-trust / composition: force full verification (docs section 8) --
    # ALL of .github/** (workflows, actions, CODEOWNERS, dependabot, issue
    # templates, governance) -> ci_changed -> full_all. A governance file must
    # not fall through to unknown/core.
    if p.startswith(".github/") or p.startswith("scripts/ci/") or p == ".gitmodules":
        return {"ci_changed"}
    if p == "Makefile" or p.startswith("mk/") or p.endswith(".mk"):
        return {"build_composition_changed"}

    # -- vendored deps / public headers / native build glue -> core --
    if p.startswith("vendor/"):
        return {"production_core_changed"}
    if p.startswith("include/"):
        return {"production_core_changed"}
    # templates/ holds app_main.c / entry.h / the compute-SDK header: compiled
    # into produced binaries -> core.
    if p.startswith("templates/"):
        return {"production_core_changed"}

    # -- production C: the trusted runtime core --
    if p.startswith("src/"):
        # The C frontend bridge/session/generation is core PLUS focused frontend
        # tests (docs section 7.2).
        if p.startswith("src/hull/frontend/"):
            return {"production_core_changed", "js_frontend_changed", "lua_frontend_changed"}
        # isolated-subsystem allowlist (Appendix C): an EXACT match narrows
        # to its subsystem fact(s) and does NOT set production_core. A shared-DB
        # file fans out to all four backends (constraint 3). Anything NOT on the
        # allowlist stays production_core -> broad (fail closed). Because these
        # focused_* flags are not APPROVED_NARROW, the live plan is still broad
        # (narrow the facts, skip nothing).
        if p in DB_POSTGRES_FILES:
            return {"native_db_changed", "db_postgres_changed"}
        if p in DB_MYSQL_FILES:
            return {"native_db_changed", "db_mysql_changed"}
        if p in DB_VALKEY_FILES:
            return {"native_db_changed", "db_valkey_changed"}
        if p in DB_DUCKDB_FILES:
            return {"native_db_changed", "db_duckdb_changed"}
        if p in DB_SHARED_FILES:
            return {"native_db_changed"} | set(_ALL_DB_BACKEND_FACTS)
        if p in GPU_FILES:
            return {"gpu_changed"}
        if p in COMPUTE_FILES:
            return {"compute_changed"}
        return {"production_core_changed"}

    # -- embedded tooling (alters bytes linked into hull -> needs fresh host) --
    if p.startswith("stdlib/cli/js/hull/source/"):
        return {"js_frontend_changed"}
    if p.startswith("stdlib/cli/lua/hull/source/"):
        return {"lua_frontend_changed"}
    # Reserved domain-compiler class (docs section 7.4): pure Query IR/types/
    # validator. Stable directory so the class exists before the code does; a Query
    # change runs focused Query/domain tests, not unrelated native suites.
    if p.startswith("stdlib/cli/js/hull/query/") or p.startswith("stdlib/cli/lua/hull/query/"):
        return {"query_changed", "domain_changed"}
    if p.startswith("stdlib/cli/") and _has_component(p, "project"):
        return {"project_discovery_changed"}
    if p.startswith("stdlib/cli/"):
        return {"tooling_changed"}
    # stdlib/context/*.md and other embedded markdown are NOT ordinary docs
    # (docs section 7.5): they are embedded tooling data.
    if p.startswith("stdlib/context/"):
        return {"tooling_changed"}
    # user-facing stdlib modules are embedded in the VFS -> tooling.
    if p.startswith("stdlib/lua/") or p.startswith("stdlib/js/"):
        return {"tooling_changed"}
    if p.startswith("stdlib/"):
        return {"tooling_changed"}

    # -- test-only fuzz harnesses + their seed corpora (not production core;
    # docs section 7.2). A source-parser fuzzer / seed change runs the matching
    # FOCUSED source fuzz, not the generic native fuzz. --
    if p == "fuzz/fuzz_js_source.c" or p.startswith("fuzz/corpus_js_source/") or p == "fuzz/js_source.dict":
        return {"js_frontend_changed", "js_fuzz_changed"}
    if p == "fuzz/fuzz_lua_source.c" or p.startswith("fuzz/corpus_lua_source/") or p == "fuzz/lua_source.dict":
        return {"lua_frontend_changed", "lua_fuzz_changed"}
    if p.startswith("fuzz/"):
        return {"native_fuzz_changed"}

    # -- test-only surfaces (docs section 7.2: test C is not automatically core).
    # KNOWN frontend / discovery test + fixture paths map to the FOCUSED frontend
    # plan so the changed tests are RUN (not skipped, not merely lint). Everything
    # else under tests/ falls to `tests_changed`, which fails closed to full_core
    # (derive_plan) until a focused generic-test job exists - a generic test
    # change must never skip the tests it modifies. --
    if p.startswith("tests/fixtures/test262/"):
        return {"js_frontend_changed"}
    if p.startswith("tests/fixtures/lua54-tests/"):
        return {"lua_frontend_changed"}
    if p.startswith("tests/hull/frontend/"):
        return {"js_frontend_changed"}
    if p.startswith("tests/hull/source/"):
        return {"lua_frontend_changed"}
    if p == "tests/e2e_project_discovery.sh":
        return {"project_discovery_changed"}
    if p.startswith("tests/"):
        return {"tests_changed"}

    if p.startswith("examples/"):
        return {"examples_changed"}
    if p.startswith("bench/"):
        # benchmarks are informational (docs section 10) - no gating fact.
        return {"examples_changed"}

    # -- ordinary documentation (the ONLY docs_only-eligible set) --
    if p.startswith("docs/") or p.startswith("site/"):
        return {"docs"}
    if p in ("README.md", "CHANGELOG.md", "CONTRIBUTING.md", "LICENSE",
             "LICENSING.md", "AGENTS.md", "BOOTSTRAP.md", "CLAUDE.md"):
        return {"docs"}

    # -- unknown -> fail closed to core (docs section 6) --
    return {"unknown"}


def derive_plan(facts):
    """Map the union of FACTS to the execution PLAN. Overlap is additive: core +
    tooling yields full_core PLUS the focused tooling tests (docs section 5)."""
    plan = {k: False for k in PLAN}
    plan["lint"] = True   # lint is cheap and always runs

    # self-trust + composition + unknown force the widest verification.
    if facts.get("ci_changed") or facts.get("build_composition_changed"):
        plan["full_all"] = True

    if facts.get("production_core_changed") or facts.get("unknown"):
        plan["full_core"] = True

    # No focused generic-test or examples job exists yet, so a generic tests/**
    # or examples/** change FAILS CLOSED to full_core - a change to a test or an
    # example must never skip the very tests it modifies (docs section 7.2 rule:
    # recognize-without-a-plan is unsafe). Known frontend test/fixture paths took
    # the focused route above and never set these facts.
    if facts.get("tests_changed") or facts.get("examples_changed"):
        plan["full_core"] = True

    if facts.get("js_frontend_changed"):
        plan["focused_js_frontend"] = True
    if facts.get("lua_frontend_changed"):
        plan["focused_lua_frontend"] = True
    if facts.get("project_discovery_changed"):
        # shared discovery runs BOTH frontends + mixed discovery (docs section 7.3).
        plan["focused_project_discovery"] = True
        plan["focused_js_frontend"] = True
        plan["focused_lua_frontend"] = True
    if facts.get("tooling_changed"):
        plan["focused_tooling"] = True
    if facts.get("query_changed"):
        plan["focused_query"] = True
    if facts.get("compute_changed") or facts.get("wasm_changed"):
        plan["focused_wasm"] = True
        plan["focused_compute"] = True
    if facts.get("gpu_changed"):
        plan["focused_gpu"] = True
    if facts.get("native_db_changed"):
        plan["focused_db"] = True
    # per-backend narrowing (Appendix C): a shared-DB change set all four backend
    # facts above, so it lights every focused_db_<backend>; an isolated backend
    # change lights only its own.
    if facts.get("db_postgres_changed"):
        plan["focused_db_postgres"] = True
    if facts.get("db_mysql_changed"):
        plan["focused_db_mysql"] = True
    if facts.get("db_valkey_changed"):
        plan["focused_db_valkey"] = True
    if facts.get("db_duckdb_changed"):
        plan["focused_db_duckdb"] = True
    if facts.get("tls_network_changed"):
        plan["focused_tls"] = True
    if facts.get("js_fuzz_changed"):
        plan["focused_js_fuzz"] = True
    if facts.get("lua_fuzz_changed"):
        plan["focused_lua_fuzz"] = True
    if facts.get("native_fuzz_changed"):
        plan["focused_native_fuzz"] = True

    # docs_only iff EVERY path was ordinary documentation and nothing else.
    non_doc = any(v for k, v in facts.items() if k != "docs")
    plan["docs_only"] = bool(facts.get("docs")) and not non_doc

    # full_all subsumes full_core.
    if plan["full_all"]:
        plan["full_core"] = True

    return plan


def classify(paths, event="pull_request", force_full=False):
    """Pure entry point (fixture-tested). `paths` is a list of repo-relative
    changed paths. Returns the full result dict."""
    facts = {k: False for k in FACTS}

    # main push / schedule / force-full -> full_all directly, no path rules.
    if event in ("push_main", "schedule") or force_full:
        plan = {k: False for k in PLAN}
        plan["lint"] = True
        plan["full_all"] = True
        plan["full_core"] = True
        reason = ("force_full" if force_full else event) + " -> full_all"
        return {"event": event, "force_full": bool(force_full), "facts": facts,
                "plan": plan, "reason": reason}

    # empty / ambiguous PR diff -> fail closed (docs section 6).
    clean = [p for p in paths if p]
    if not clean:
        plan = {k: False for k in PLAN}
        plan["lint"] = True
        plan["full_all"] = True
        plan["full_core"] = True
        return {"event": event, "force_full": False, "facts": facts,
                "plan": plan, "reason": "empty diff -> full_all"}

    for p in clean:
        for f in classify_path(p):
            facts[f] = True

    plan = derive_plan(facts)
    return {"event": event, "force_full": False, "facts": facts,
            "plan": plan, "reason": "paths classified"}


def parse_nul_paths(raw):
    """Validate + decode a NUL-delimited path stream (git diff --name-only -z).
    Returns (paths, None) on success or (None, reason) if the stream is MALFORMED,
    in which case the caller fails closed to full_all. `git diff -z` terminates
    EVERY path (including the last) with a NUL and emits repo-relative, canonical
    paths, so a well-formed stream: ends in NUL, has no empty interior token, and
    no absolute or `.`/`..`-component path. Spaces, tabs, and newlines inside a
    path are VALID (that is exactly why -z is used) and are accepted."""
    if raw == b"":
        return [], None                          # empty stream -> empty diff (full_all downstream)
    if not raw.endswith(b"\x00"):
        return None, "stream not NUL-terminated (truncated diff?)"
    tokens = raw.split(b"\x00")
    tokens.pop()                                 # drop the trailing empty from the terminal NUL
    paths = []
    for tok in tokens:
        if tok == b"":
            return None, "empty interior path"
        p = tok.decode("utf-8", "surrogateescape")
        if p.startswith("/"):
            return None, "absolute path: %r" % p
        comps = p.split("/")
        # Reject any noncanonical component: empty (`docs//x.md`, a trailing
        # `docs/`), `.`, or `..`. `git diff` emits canonical repo-relative paths,
        # so anything else is a malformed/untrusted stream -> fail closed.
        if "" in comps or "." in comps or ".." in comps:
            return None, "noncanonical path component: %r" % p
        paths.append(p)
    return paths, None


def _fail_closed(reason):
    facts = {k: False for k in FACTS}
    plan = {k: False for k in PLAN}
    plan["lint"] = True
    plan["full_all"] = True
    plan["full_core"] = True
    return {"event": "unknown", "force_full": False, "facts": facts,
            "plan": plan, "reason": reason}


def _emit_github_output(result):
    """Write flat key=value lines to $GITHUB_OUTPUT for the orchestrator (Slice
    2). Also emits a compact plan_json. No-op when the var is unset."""
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    lines = []
    for k, v in sorted(result["facts"].items()):
        lines.append("facts_%s=%s" % (k, "true" if v else "false"))
    for k, v in sorted(result["plan"].items()):
        lines.append("plan_%s=%s" % (k, "true" if v else "false"))
    lines.append("plan_json=" + json.dumps(result["plan"], sort_keys=True, separators=(",", ":")))
    lines.append("event=" + str(result["event"]))
    with open(path, "a", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main(argv):
    ap = argparse.ArgumentParser(description="Change-aware CI classifier.")
    ap.add_argument("--event", default="pull_request",
                    choices=["pull_request", "push_main", "schedule"],
                    help="the CI event; push_main/schedule force full_all.")
    ap.add_argument("--force-full", action="store_true",
                    help="force full_all regardless of paths (dispatch input).")
    ap.add_argument("--paths-from", default=None,
                    help="read NUL-delimited paths from this file instead of stdin.")
    args = ap.parse_args(argv)

    # main push / schedule / force-full -> full_all directly; the diff is
    # irrelevant, so do not even read (let alone trust) the path stream.
    if args.event in ("push_main", "schedule") or args.force_full:
        result = classify([], event=args.event, force_full=args.force_full)
        print(json.dumps(result, sort_keys=True, indent=2))
        _emit_github_output(result)
        return 0

    try:
        if args.paths_from:
            with open(args.paths_from, "rb") as f:
                raw = f.read()
        else:
            raw = sys.stdin.buffer.read()
    except Exception as e:   # any read failure -> fail closed.
        result = _fail_closed("input read failure: %s -> full_all" % e)
        print(json.dumps(result, sort_keys=True, indent=2))
        _emit_github_output(result)
        return 0

    paths, perr = parse_nul_paths(raw)
    if perr is not None:     # malformed stream -> fail closed.
        result = _fail_closed("malformed diff stream: %s -> full_all" % perr)
    else:
        try:
            result = classify(paths, event=args.event, force_full=args.force_full)
        except Exception as e:   # any rule failure -> fail closed.
            result = _fail_closed("classifier error: %s -> full_all" % e)

    print(json.dumps(result, sort_keys=True, indent=2))
    _emit_github_output(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
