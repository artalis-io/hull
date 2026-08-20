#!/bin/sh
# fetch_test262.sh - reproducibly vendor a pinned, parser-scoped, MODULE-only Test262 subset.
#
# Run by a MAINTAINER, never by CI (CI uses only the committed subset and is fully offline).
# Design: docs/js_test262_design.md. Selects test/language/** cases whose `flags` contain
# `module`, excludes resolution/runtime-only negatives and incompatible goal flags, generates
# manifest.json (upstream facts only) + MANIFEST.sha256, copies the cases verbatim (preserving
# Test262 copyright headers) + the upstream LICENSE, and prints a selection report.
#
# Dependencies: git, python3 (>= 3.8) with PyYAML (>= 5.x). NOT auto-installed.
#
# Usage: sh scripts/fetch_test262.sh [--sha <40-hex>] [--out <dir>]
#   --sha  pinned commit (default: PINNED_SHA below). A SPECIFIC commit, never a branch.
#   --out  destination (default: tests/fixtures/test262). Written atomically (temp then replace).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

# The pinned Test262 commit. A snapshot -- bump deliberately, review the resulting diff.
PINNED_SHA="3655e7464de3d52643ecddd4b5f9f4f3e7f62398"
SELECTION_RULES_VERSION=2
SCHEMA_VERSION=1
REPO="https://github.com/tc39/test262"

OUT="tests/fixtures/test262"
SHA="$PINNED_SHA"
while [ $# -gt 0 ]; do
    case "$1" in
        --sha) SHA="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "fetch_test262: unknown arg: $1" >&2; exit 2 ;;
    esac
done

command -v git >/dev/null 2>&1 || { echo "fetch_test262: git not found" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "fetch_test262: python3 not found" >&2; exit 1; }
python3 -c 'import yaml' 2>/dev/null || { echo "fetch_test262: PyYAML not found (pip install pyyaml)" >&2; exit 1; }
case "$SHA" in
    *[!0-9a-f]* | "") echo "fetch_test262: --sha must be 40 lowercase hex" >&2; exit 2 ;;
esac
[ "${#SHA}" -eq 40 ] || { echo "fetch_test262: --sha must be 40 hex chars" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/test262"
STAGE="$WORK/stage"

echo "fetch_test262: fetching $REPO @ $SHA (shallow, by exact SHA) ..."
git init -q "$SRC"
git -C "$SRC" remote add origin "$REPO"
# Fetch the EXACT pinned SHA -- never the moving default branch.
git -C "$SRC" fetch -q --depth 1 origin "$SHA"
git -C "$SRC" checkout -q FETCH_HEAD
HEAD_SHA="$(git -C "$SRC" rev-parse HEAD)"
[ "$HEAD_SHA" = "$SHA" ] || { echo "fetch_test262: HEAD $HEAD_SHA != pinned $SHA" >&2; exit 1; }

# Selection + manifest generation + verbatim case copy, all in one deterministic Python pass.
LC_ALL=C SRC="$SRC" STAGE="$STAGE" SHA="$SHA" \
  SCHEMA_VERSION="$SCHEMA_VERSION" SELECTION_RULES_VERSION="$SELECTION_RULES_VERSION" \
  python3 - <<'PY'
import os, sys, json, hashlib, shutil, io

src = os.environ["SRC"]; stage = os.environ["STAGE"]; sha = os.environ["SHA"]
schema = int(os.environ["SCHEMA_VERSION"]); rules = int(os.environ["SELECTION_RULES_VERSION"])
import yaml

lang_root = os.path.join(src, "test", "language")
cases_out = os.path.join(stage, "cases")
os.makedirs(cases_out, exist_ok=True)

# Closed target-profile: syntax-proposal features the vendored QuickJS parse target does not
# implement (see the exclusion comment below). Fixed priority order -> a case carrying several
# gets ONE deterministic primary reason. Runtime built-ins are intentionally absent.
TARGET_PROFILE_ORDER = [
    "import-defer", "import-attributes", "json-modules", "import-assertions",
    "source-phase-imports", "dynamic-import-phase", "decorators",
    "explicit-resource-management", "iterator-helpers", "arbitrary-module-namespace-names",
]
TARGET_PROFILE = set(TARGET_PROFILE_ORDER)

# Extract the /*--- ... ---*/ YAML frontmatter. FAIL (not skip) on malformed/unknown.
def frontmatter(path, raw):
    s = raw.decode("utf-8", "surrogateescape")
    a = s.find("/*---")
    if a < 0:
        return None  # no frontmatter (e.g. a _FIXTURE.js) -> caller decides
    b = s.find("---*/", a)
    if b < 0:
        raise ValueError("unterminated frontmatter: " + path)
    body = s[a+5:b]
    try:
        fm = yaml.safe_load(body)
    except Exception as e:
        raise ValueError("malformed frontmatter YAML in %s: %s" % (path, e))
    if fm is None:
        fm = {}
    if not isinstance(fm, dict):
        raise ValueError("frontmatter is not a mapping: " + path)
    return fm

selected = []
excl = {}  # reason -> count
def exclude(reason):
    excl[reason] = excl.get(reason, 0) + 1

total_candidates = 0
for dirpath, dirnames, filenames in os.walk(lang_root):
    dirnames.sort()
    for fn in sorted(filenames):
        if not fn.endswith(".js"):
            continue
        full = os.path.join(dirpath, fn)
        if os.path.islink(full):
            raise ValueError("symlink in upstream tree (rejected): " + full)
        rel = os.path.relpath(full, os.path.join(src, "test"))  # e.g. language/module-code/x.js
        # _FIXTURE.js files are module dependencies imported at resolution/runtime -- not test
        # cases, and parse-only conformance never resolves imports. Excluded (not a candidate).
        if fn.endswith("_FIXTURE.js"):
            continue
        total_candidates += 1
        with open(full, "rb") as f:
            raw = f.read()
        fm = frontmatter(rel, raw)
        if fm is None:
            raise ValueError("candidate has no frontmatter: " + rel)
        flags = fm.get("flags", []) or []
        if not isinstance(flags, list):
            raise ValueError("flags is not a list: " + rel)
        # Only MODULE-goal cases participate.
        if "module" not in flags:
            exclude("not-module-goal"); continue
        # Incompatible goal flags: `raw` bypasses the module wrapper; `noStrict` conflicts with
        # module (always strict). Reject rather than pretend they are modules.
        if "raw" in flags:
            exclude("raw-incompatible"); continue
        if "noStrict" in flags:
            exclude("nostrict-incompatible"); continue
        neg = fm.get("negative")
        if neg is not None:
            if not isinstance(neg, dict) or "phase" not in neg:
                raise ValueError("malformed negative: " + rel)
            phase = neg.get("phase")
            if phase in ("resolution", "runtime"):
                exclude("resolution-runtime-negative"); continue
            if phase != "parse":
                raise ValueError("unknown negative.phase %r in %s" % (phase, rel))
            neg_out = {"phase": "parse", "type": neg.get("type")}
        else:
            neg_out = None
        feats = fm.get("features", []) or []
        if not isinstance(feats, list):
            raise ValueError("features is not a list: " + rel)
        # TARGET-PROFILE exclusion: syntax-proposal features the vendored QuickJS parse target does
        # NOT implement, so Test262's expectation and the compile-only oracle would disagree
        # (target-version divergence). Excluding them keeps post-selection divergence at zero
        # WITHOUT hiding a Hull parser gap. This set is PARSE-goal only: it never lists a runtime
        # built-in (e.g. promise-with-resolvers/iterator-helpers as globals) -- compile-only
        # parsing does not depend on whether the engine implements a global method. Hull-SCOPE
        # declines (generators, async generators, do-while, private members) are deliberately NOT
        # excluded: they stay in the corpus as clean js.unsupported outcomes.
        prof = TARGET_PROFILE & set(feats)
        if prof:
            # deterministic primary reason = first by the fixed priority order
            exclude("target-profile:" + next(f for f in TARGET_PROFILE_ORDER if f in prof)); continue
        selected.append({
            "path": rel,                       # relative to cases/ (mirrors test/ layout)
            "goal": "module",
            "flags": sorted(flags),
            "features": sorted(feats),
            "negative": neg_out,
            "source_hash": hashlib.sha256(raw).hexdigest(),
            "_raw": raw,
        })

# Deterministic byte-order sort (LC_ALL=C is set in the environment; sort by path bytes).
selected.sort(key=lambda c: c["path"].encode("utf-8"))
paths = [c["path"] for c in selected]
if len(set(paths)) != len(paths):
    raise ValueError("duplicate case paths")

# Copy cases verbatim (preserving Test262 copyright headers), reject path escapes.
total_bytes = 0
for c in selected:
    rel = c["path"]
    if rel.startswith("/") or ".." in rel.split("/"):
        raise ValueError("path escape: " + rel)
    dst = os.path.join(cases_out, rel)
    real = os.path.realpath(dst)
    if not (real == os.path.realpath(cases_out) or real.startswith(os.path.realpath(cases_out) + os.sep)):
        raise ValueError("path escapes cases/: " + rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "wb") as f:
        f.write(c["_raw"])
    total_bytes += len(c["_raw"])

# manifest.json: upstream facts ONLY (no Hull policy, no fetch date). Canonical bytes.
manifest = {
    "schema_version": schema,
    "upstream_sha": sha,
    "selection_rules_version": rules,
    "count": len(selected),
    "cases": [{k: c[k] for k in ("path", "goal", "flags", "features", "negative", "source_hash")} for c in selected],
}
# The on-disk manifest bytes ARE the canonical bytes (trailing newline included), and
# MANIFEST.sha256 is the sha256 of those EXACT file bytes, so the C leg can hash the whole file
# and compare directly (no ambiguity about a trailing newline).
manifest_bytes = json.dumps(manifest, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii") + b"\n"
canon = manifest_bytes   # kept name for the report field below
with open(os.path.join(stage, "manifest.json"), "wb") as f:
    f.write(manifest_bytes)
with open(os.path.join(stage, "MANIFEST.sha256"), "w") as f:
    f.write(hashlib.sha256(manifest_bytes).hexdigest() + "\n")

# Report (informational; also drives the maintainer's pre-commit review).
pos = sum(1 for c in selected if c["negative"] is None)
pneg = sum(1 for c in selected if c["negative"] is not None)
feat_counts = {}
for c in selected:
    for ft in c["features"]:
        feat_counts[ft] = feat_counts.get(ft, 0) + 1
report = {
    "pinned_sha": sha,
    "selection_rules_version": rules,
    "candidates_scanned": total_candidates,
    "selected_cases": len(selected),
    "total_case_bytes": total_bytes,
    "positive": pos,
    "parse_negative": pneg,
    "excluded_by_reason": dict(sorted(excl.items())),
    "top_features": dict(sorted(feat_counts.items(), key=lambda kv: (-kv[1], kv[0]))[:25]),
    "distinct_features": len(feat_counts),
    "manifest_sha256": hashlib.sha256(canon).hexdigest(),
}
with open(os.path.join(stage, "REPORT.json"), "w") as f:
    json.dump(report, f, indent=2, sort_keys=True)
    f.write("\n")
print(json.dumps(report, indent=2, sort_keys=True))
PY

# Copy the upstream LICENSE verbatim + write informational UPSTREAM.md (date NOT hashed).
cp "$SRC/LICENSE" "$STAGE/LICENSE"
# Pull the final selected counts out of the generated selection report for the summary line.
SEL_SUMMARY="$(python3 -c 'import json,sys;r=json.load(open(sys.argv[1]));print("%d cases (%d positive, %d parse-negative), %d case bytes" % (r["selected_cases"],r["positive"],r["parse_negative"],r["total_case_bytes"]))' "$STAGE/REPORT.json")"
{
    echo "# Test262 vendored subset (provenance)"
    echo
    echo "- upstream: $REPO"
    echo "- pinned commit: $SHA"
    echo "- fetched: $(date -u +%Y-%m-%dT%H:%M:%SZ) (informational only; NOT part of any hash)"
    echo "- selection-rules version: $SELECTION_RULES_VERSION"
    echo "- selected: $SEL_SUMMARY"
    echo "- selection: test/language/** cases whose flags contain \`module\`; excludes"
    echo "  resolution/runtime-only negatives, raw/noStrict-incompatible goal flags, _FIXTURE.js,"
    echo "  and the closed target-profile (syntax-proposal features the QuickJS parse target lacks:"
    echo "  import-defer, import-attributes, json-modules, import-assertions, source-phase-imports,"
    echo "  dynamic-import-phase, decorators, explicit-resource-management, iterator-helpers,"
    echo "  arbitrary-module-namespace-names). Hull-scope declines (generators, do-while, private"
    echo "  members) are RETAINED as clean js.unsupported outcomes."
    echo "- generated by scripts/fetch_test262.sh (never run in CI)."
} > "$STAGE/UPSTREAM.md"

# Atomic replace: write the whole subset to a temp sibling, then swap into place on success.
DEST_PARENT="$(dirname "$OUT")"
mkdir -p "$DEST_PARENT"
TMP_DEST="$(mktemp -d "$DEST_PARENT/.test262.XXXXXX")"
cp -R "$STAGE/." "$TMP_DEST/"
rm -rf "$OUT"
mv "$TMP_DEST" "$OUT"
echo "fetch_test262: wrote $OUT (cases + manifest.json + MANIFEST.sha256 + LICENSE + UPSTREAM.md)"
echo "fetch_test262: NOTE - expectations.json (Hull policy) is authored + reviewed by hand, not generated."
