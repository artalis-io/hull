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

check("other production C", ["src/hull/cap/db.c"],
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
      ["stdlib/cli/js/hull/source/parser.js", "src/hull/cap/db.c"],
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

# ---- determinism: same input -> identical result ----

r1 = classify(["stdlib/cli/js/hull/source/parser.js", "docs/x.md"])
r2 = classify(["docs/x.md", "stdlib/cli/js/hull/source/parser.js"])
if r1["plan"] == r2["plan"] and r1["facts"] == r2["facts"]:
    _pass += 1
else:
    _fail += 1
    print("FAIL: determinism/order-independence")

print("classify_changes fixtures: %d passed, %d failed" % (_pass, _fail))
sys.exit(1 if _fail else 0)
