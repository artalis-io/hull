#!/usr/bin/env python3
# test_classify_changes.py - fixture tests for the change-aware CI classifier.
#
# Pure Python, no GitHub Actions dependency (docs/ci_architecture_design.md
# sections 6 + 19). Run: python3 scripts/ci/test_classify_changes.py
#
# Covers the section-19 fixture matrix + fail-closed behavior + the ratified
# addition: renames across a trust boundary (git diff --no-renames shows a rename
# as delete(old)+add(new), so BOTH paths classify and the BROADER plan wins - a
# rename can never escape into a narrower plan). The ci-success GATE cases
# (required-job skipped, allowed-job skipped, cancellation, matrix aggregates)
# belong to Slice 2's gate, not this classifier, and are deferred there.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classify_changes import classify  # noqa: E402

_pass = 0
_fail = 0


def check(name, paths, want_true=(), want_false=(), event="pull_request", force_full=False):
    """Assert the given plan flags are True / False for classify(paths,...)."""
    global _pass, _fail
    r = classify(list(paths), event=event, force_full=force_full)
    plan = r["plan"]
    problems = []
    for k in want_true:
        if not plan.get(k):
            problems.append("expected plan.%s=True" % k)
    for k in want_false:
        if plan.get(k):
            problems.append("expected plan.%s=False" % k)
    if problems:
        _fail += 1
        print("FAIL: %s :: %s :: reason=%s" % (name, "; ".join(problems), r["reason"]))
    else:
        _pass += 1


# ---- section 19: canonical fixtures ----

check("js frontend only", ["stdlib/cli/js/hull/source/parser.js"],
      want_true=["focused_js_frontend", "lint"],
      want_false=["full_core", "full_all", "docs_only", "focused_lua_frontend"])

check("lua frontend only", ["stdlib/cli/lua/hull/source/parser.lua"],
      want_true=["focused_lua_frontend"],
      want_false=["full_core", "full_all", "docs_only", "focused_js_frontend"])

check("query compiler only", ["stdlib/cli/lua/hull/query/lower.lua"],
      want_true=["focused_query"],
      want_false=["full_core", "full_all", "focused_js_frontend", "focused_lua_frontend"])

check("test-only JS fuzzer C", ["fuzz/fuzz_js_source.c"],
      want_true=["focused_js_frontend", "focused_js_fuzz"],
      want_false=["full_core", "full_all", "focused_native_fuzz"])

check("test-only Lua fuzzer C", ["fuzz/fuzz_lua_source.c"],
      want_true=["focused_lua_frontend", "focused_lua_fuzz"],
      want_false=["full_core", "full_all"])

check("other native fuzzer C", ["fuzz/fuzz_pgwire.c"],
      want_true=["focused_native_fuzz"],
      want_false=["full_core", "full_all", "focused_js_frontend"])

check("C frontend bridge", ["src/hull/frontend/js_session.c"],
      want_true=["full_core", "focused_js_frontend", "focused_lua_frontend"],
      want_false=["full_all", "docs_only"])

check("shared native header", ["include/hull/cap/db.h"],
      want_true=["full_core"],
      want_false=["full_all", "docs_only"])

check("other production C", ["src/hull/serve.c"],
      want_true=["full_core"], want_false=["full_all"])

check("vendored dep bump", ["vendor/lua/lua.h"],
      want_true=["full_core"], want_false=["full_all"])

check("templates native glue", ["templates/app_main.c"],
      want_true=["full_core"], want_false=["full_all"])

check("workflow edit -> full_all", [".github/workflows/ci.yml"],
      want_true=["full_all", "full_core"])

check("classifier self-edit -> full_all", ["scripts/ci/classify_changes.py"],
      want_true=["full_all", "full_core"])

check("classifier fixture edit -> full_all", ["scripts/ci/test_classify_changes.py"],
      want_true=["full_all", "full_core"])

check("Makefile edit -> full_all", ["Makefile"], want_true=["full_all", "full_core"])
check("mk fragment edit -> full_all", ["mk/tests.mk"], want_true=["full_all", "full_core"])
check("gitmodules edit -> full_all", [".gitmodules"], want_true=["full_all", "full_core"])

check("true docs only", ["docs/ci_architecture_design.md", "README.md"],
      want_true=["docs_only", "lint"],
      want_false=["full_core", "full_all", "focused_tooling"])

check("site docs only", ["site/index.html"],
      want_true=["docs_only"], want_false=["full_core", "full_all"])

check("embedded markdown is NOT docs-only", ["stdlib/context/orientation.md"],
      want_true=["focused_tooling"],
      want_false=["docs_only", "full_core", "full_all"])

check("user-facing stdlib is tooling", ["stdlib/lua/hull/template.lua"],
      want_true=["focused_tooling"], want_false=["full_core", "full_all", "docs_only"])

check("project discovery runs both frontends", ["stdlib/cli/lua/hull/project/model.lua"],
      want_true=["focused_project_discovery", "focused_js_frontend", "focused_lua_frontend"],
      want_false=["full_core", "full_all"])

check("mixed tooling + core -> full_core plus focused tooling",
      ["stdlib/cli/js/hull/source/parser.js", "src/hull/serve.c"],
      want_true=["full_core", "focused_js_frontend"],
      want_false=["full_all", "docs_only"])

# ---- ratified addition: renames across trust boundaries (delete+add) ----

check("rename docs->core: broader (core) wins",
      ["docs/moved.md", "src/hull/moved.c"],
      want_true=["full_core"], want_false=["full_all", "docs_only"])

check("rename tooling->core: core wins",
      ["stdlib/cli/js/hull/source/moved.js", "src/hull/moved.c"],
      want_true=["full_core", "focused_js_frontend"], want_false=["full_all"])

check("rename core->docs: deleted core path still classifies",
      ["src/hull/old.c", "docs/new.md"],
      want_true=["full_core"], want_false=["docs_only"])

check("rename core->workflow: full_all wins",
      ["src/hull/old.c", ".github/workflows/new.yml"],
      want_true=["full_all", "full_core"])

check("rename docs->docs stays docs_only",
      ["docs/a.md", "docs/b.md"],
      want_true=["docs_only"], want_false=["full_core", "full_all"])

# ---- fail-closed: unknown / adversarial / empty / event overrides ----

check("unknown path -> core", ["weirddir/thing.xyz"],
      want_true=["full_core"], want_false=["docs_only"])

check("adversarial path with spaces -> unknown -> core", ["my sneaky file.md"],
      want_true=["full_core"], want_false=["docs_only"])

check("adversarial path with newline -> unknown -> core", ["a\nb/c.d"],
      want_true=["full_core"], want_false=["docs_only"])

check("deletion of a core file -> core", ["src/hull/gone.c"],
      want_true=["full_core"])

check("empty PR diff -> full_all", [],
      want_true=["full_all", "full_core"])

check("only-empty-token diff -> full_all", ["", ""],
      want_true=["full_all", "full_core"])

check("push to main -> full_all regardless of paths",
      ["docs/only.md"], event="push_main",
      want_true=["full_all", "full_core"], want_false=["docs_only"])

check("schedule -> full_all regardless of paths",
      ["stdlib/cli/js/hull/source/parser.js"], event="schedule",
      want_true=["full_all", "full_core"])

check("force_full -> full_all regardless of paths",
      ["docs/only.md"], force_full=True,
      want_true=["full_all", "full_core"], want_false=["docs_only"])

# ---- Gap 1: generic tests/examples fail closed to full_core, known frontend
#      test/fixture paths take the focused route (never lint-only) ----

check("generic unit test -> full_core", ["tests/test_arena.c"],
      want_true=["full_core"], want_false=["full_all", "docs_only"])

check("generic cap test -> full_core", ["tests/hull/cap/test_db.c"],
      want_true=["full_core"], want_false=["full_all", "docs_only"])

check("generic e2e script -> full_core", ["tests/e2e_http.sh"],
      want_true=["full_core"], want_false=["full_all", "docs_only"])

check("example app -> full_core", ["examples/todo/app.lua"],
      want_true=["full_core"], want_false=["full_all", "docs_only"])

check("frontend C test -> focused js (not full_core)",
      ["tests/hull/frontend/test_js_conformance.c"],
      want_true=["focused_js_frontend"], want_false=["full_core", "full_all"])

check("lua source C test -> focused lua (not full_core)",
      ["tests/hull/source/test_lua_source.c"],
      want_true=["focused_lua_frontend"], want_false=["full_core", "full_all"])

check("test262 fixture -> focused js (not full_core)",
      ["tests/fixtures/test262/manifest.json"],
      want_true=["focused_js_frontend"], want_false=["full_core", "full_all"])

check("lua54 fixture -> focused lua (not full_core)",
      ["tests/fixtures/lua54-tests/cases/math.lua"],
      want_true=["focused_lua_frontend"], want_false=["full_core", "full_all"])

check("discovery E2E -> focused discovery (not full_core)",
      ["tests/e2e_project_discovery.sh"],
      want_true=["focused_project_discovery", "focused_js_frontend", "focused_lua_frontend"],
      want_false=["full_core", "full_all"])

check("js fuzz seed -> focused js fuzz", ["fuzz/corpus_js_source/abc"],
      want_true=["focused_js_frontend", "focused_js_fuzz"], want_false=["full_core", "full_all"])

check("governance .github file -> full_all", [".github/CODEOWNERS"],
      want_true=["full_all", "full_core"])

check(".github issue template -> full_all", [".github/ISSUE_TEMPLATE/bug.md"],
      want_true=["full_all", "full_core"])

# ---- Gap 2: byte-decoder / CLI validation via subprocess ----

import subprocess  # noqa: E402

_CLI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "classify_changes.py")


def run_cli(raw_bytes, event="pull_request", extra=()):
    proc = subprocess.run([sys.executable, _CLI, "--event", event, *extra],
                          input=raw_bytes, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return json.loads(proc.stdout.decode("utf-8"))


def check_cli(name, raw, want_true=(), want_false=(), event="pull_request", extra=()):
    global _pass, _fail
    r = run_cli(raw, event=event, extra=extra)
    plan = r["plan"]
    problems = []
    for k in want_true:
        if not plan.get(k):
            problems.append("expected plan.%s=True" % k)
    for k in want_false:
        if plan.get(k):
            problems.append("expected plan.%s=False" % k)
    if problems:
        _fail += 1
        print("FAIL(cli): %s :: %s :: reason=%s" % (name, "; ".join(problems), r.get("reason")))
    else:
        _pass += 1


# well-formed NUL streams classify normally through the real byte path
check_cli("cli: js frontend", b"stdlib/cli/js/hull/source/parser.js\x00",
          want_true=["focused_js_frontend"], want_false=["full_core", "full_all"])
check_cli("cli: docs only", b"docs/x.md\x00",
          want_true=["docs_only"], want_false=["full_core", "full_all"])
check_cli("cli: two paths", b"docs/x.md\x00src/hull/a.c\x00",
          want_true=["full_core"], want_false=["docs_only"])

# malformed streams fail closed to full_all
check_cli("cli: missing terminal NUL -> full_all", b"src/hull/x.c",
          want_true=["full_all", "full_core"])
check_cli("cli: empty interior path -> full_all", b"a.c\x00\x00b.c\x00",
          want_true=["full_all", "full_core"])
check_cli("cli: absolute path -> full_all", b"/etc/passwd\x00",
          want_true=["full_all"])
check_cli("cli: dotdot component -> full_all", b"src/../secret.c\x00",
          want_true=["full_all"])
check_cli("cli: leading ./ component -> full_all", b"./src/a.c\x00",
          want_true=["full_all"])
check_cli("cli: empty interior component (docs//x) -> full_all", b"docs//x.md\x00",
          want_true=["full_all"], want_false=["docs_only"])
check_cli("cli: trailing slash (docs/) -> full_all", b"docs/\x00",
          want_true=["full_all"], want_false=["docs_only"])

# spaces / tabs / newlines inside a path are VALID (why -z is used) - accepted,
# NOT split, NOT rejected. A newline-bearing single path stays one path.
check_cli("cli: space in path accepted", b"my sneaky file.md\x00",
          want_true=["full_core"], want_false=["full_all", "docs_only"])
check_cli("cli: tab in path accepted", b"src/a\tb.c\x00",
          want_true=["full_core"], want_false=["full_all"])
check_cli("cli: newline in path is one path (NUL-only split)",
          b"src/weird\nname.c\x00", want_true=["full_core"], want_false=["full_all"])

# empty stream -> empty diff -> full_all; read failure -> full_all
check_cli("cli: empty stream -> full_all", b"", want_true=["full_all"])
check_cli("cli: read failure (missing --paths-from) -> full_all", b"",
          want_true=["full_all"], extra=("--paths-from", "/nonexistent/xyz/does-not-exist"))

# event overrides ignore the stream entirely
check_cli("cli: push_main ignores stream -> full_all", b"docs/only.md\x00",
          event="push_main", want_true=["full_all"], want_false=["docs_only"])
check_cli("cli: force-full ignores stream -> full_all", b"docs/only.md\x00",
          extra=("--force-full",), want_true=["full_all"], want_false=["docs_only"])

# ---- determinism: same input -> identical result ----

r1 = classify(["stdlib/cli/js/hull/source/parser.js", "docs/x.md"])
r2 = classify(["docs/x.md", "stdlib/cli/js/hull/source/parser.js"])
if r1["plan"] == r2["plan"] and r1["facts"] == r2["facts"]:
    _pass += 1
else:
    _fail += 1
    print("FAIL: determinism/order-independence")

# ---- Slice 4 checkpoint 2: source-inventory machine-check (Appendix C.7) ----
# Grounds the isolated-subsystem allowlist in the ACTUAL repository so it cannot
# silently rot: every allowlisted path exists, maps to EXACTLY its subsystem
# fact(s), every unlisted src/** stays broad, shared-DB fans out to all backends,
# and a deleted/renamed allowlisted file classifies safely via both diff paths.
import classify_changes as _cc  # noqa: E402

_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def ck(name, cond):
    global _pass, _fail
    if cond:
        _pass += 1
    else:
        _fail += 1
        print("FAIL(machine-check): %s" % name)


_ALLOWLIST_EXPECT = [
    (_cc.DB_POSTGRES_FILES, {"native_db_changed", "db_postgres_changed"}),
    (_cc.DB_MYSQL_FILES,    {"native_db_changed", "db_mysql_changed"}),
    (_cc.DB_VALKEY_FILES,   {"native_db_changed", "db_valkey_changed"}),
    (_cc.DB_DUCKDB_FILES,   {"native_db_changed", "db_duckdb_changed"}),
    (_cc.DB_SHARED_FILES,   {"native_db_changed", "db_postgres_changed",
                             "db_mysql_changed", "db_valkey_changed", "db_duckdb_changed"}),
    (_cc.GPU_FILES,         {"gpu_changed"}),
    (_cc.COMPUTE_FILES,     {"compute_changed"}),
]

# 1. every allowlisted file EXISTS at HEAD (exact path resolves to a regular file).
for _files, _ in _ALLOWLIST_EXPECT:
    for _f in sorted(_files):
        ck("allowlist file exists: %s" % _f, os.path.isfile(os.path.join(_REPO, _f)))

# 2. every allowlisted file maps to EXACTLY its intended subsystem fact(s).
for _files, _want in _ALLOWLIST_EXPECT:
    for _f in sorted(_files):
        ck("exact map: %s" % _f, _cc.classify_path(_f) == _want)

# 3. every UNLISTED src/** path stays broad (production_core). Representative real
#    core files NOT on any allowlist + a synthetic new file must all be core.
_ALLOWLISTED = set().union(*[fs for fs, _ in _ALLOWLIST_EXPECT])
for _f in ["src/hull/serve.c", "src/hull/cap/http.c", "src/hull/cap/crypto.c",
           "src/hull/cap/db_sqlite.c", "src/hull/manifest.c", "src/hull/cap/fs.c",
           "src/hull/cap/db_postgres.h", "src/hull/cap/brand_new_backend.c"]:
    ck("unlisted src is broad: %s" % _f,
       _f not in _ALLOWLISTED and _cc.classify_path(_f) == {"production_core_changed"})

# 4. shared-DB files select ALL db backends (fan-out, constraint 3).
for _f in sorted(_cc.DB_SHARED_FILES):
    facts = _cc.classify_path(_f)
    ck("shared-db fans out: %s" % _f,
       {"db_postgres_changed", "db_mysql_changed", "db_valkey_changed",
        "db_duckdb_changed"} <= facts)

# 5. delete/rename safety through BOTH diff paths.
#    A rename = delete(old allowlisted) + add(new non-allowlisted). --no-renames
#    reports both; the new path is not yet allowlisted -> production_core -> the
#    union is BROAD (full_core). Neither path mis-narrows.
check("rename allowlisted->new: union broad (paths path)",
      ["src/hull/cap/db_postgres.c", "src/hull/cap/db_postgres_v2.c"],
      want_true=["full_core"])
check_cli("cli rename allowlisted->new: union broad",
          b"src/hull/cap/db_postgres.c\x00src/hull/cap/db_postgres_v2.c\x00",
          want_true=["full_core"])
#    A pure DELETE of an allowlisted file still classifies by name to its subsystem
#    facts, but the plan is NOT narrow-approved -> BROAD -> nothing skips (safe).
_del = classify(["src/hull/cap/db_postgres.c"])["plan"]
ck("pure delete of allowlisted stays safe (no full_core, focused only)",
   _del["focused_db_postgres"] and _del["focused_db"] and not _del["full_core"])
#    force-full path ignores the stream entirely -> full_all (broad), regardless of
#    whether an allowlisted file was renamed/deleted.
check("rename under force-full -> full_all",
      ["src/hull/cap/db_postgres.c", "src/hull/cap/db_postgres_v2.c"],
      want_true=["full_all"], force_full=True)

# 6. EVERY tracked src/** file is classified correctly (Slice 4 checkpoint 3,
#    constraint 4): an allowlisted file -> EXACTLY its subsystem facts; the
#    deliberate C frontend bridge -> production_core PLUS the js/lua frontend
#    facts; every OTHER tracked source -> a set INCLUDING production_core_changed
#    (so an unrecognized/new production file always runs the broad suite).
import subprocess  # noqa: E402
_ALLOW_MAP = {}
for _files, _want in _ALLOWLIST_EXPECT:
    for _f in _files:
        _ALLOW_MAP[_f] = _want
try:
    _tracked = subprocess.run(["git", "-C", _REPO, "ls-files", "src"],
                              capture_output=True, text=True, check=True).stdout.split()
except Exception as _e:                                   # pragma: no cover
    _tracked = []
    ck("git ls-files src succeeded", False)
ck("git ls-files src returned files", len(_tracked) > 0)
_FRONTEND_BRIDGE = {"production_core_changed", "js_frontend_changed", "lua_frontend_changed"}
for _f in _tracked:
    facts = _cc.classify_path(_f)
    if _f in _ALLOW_MAP:
        ck("tracked allowlisted exact: %s" % _f, facts == _ALLOW_MAP[_f])
    elif _f.startswith("src/hull/frontend/"):
        ck("tracked frontend bridge keeps frontend facts: %s" % _f, facts == _FRONTEND_BRIDGE)
    else:
        ck("tracked src includes production_core: %s" % _f, "production_core_changed" in facts)

print("classify_changes fixtures: %d passed, %d failed" % (_pass, _fail))
sys.exit(1 if _fail else 0)
