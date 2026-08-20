#!/bin/sh
# fetch_lua_tests.sh - reproducibly vendor the official, release-matched Lua 5.4.7 test suite
# as a pinned, parser-scoped corpus (docs/lua_official_tests_design.md).
#
# Run by a MAINTAINER, never by CI (CI uses only the committed subset and is fully offline).
# Downloads the exact 5.4.7 archive, verifies the OFFICIAL SHA-256, extracts, selects regular
# .lua files, copies them verbatim, and generates manifest.json (upstream facts only) +
# MANIFEST.sha256 + LICENSE (the Lua MIT license) + UPSTREAM.md, atomically.
#
# The C source libs (libs/*.c, ltests/*.c) are for the EXECUTION suite (a separate future job);
# only regular .lua files are selected here.
#
# Dependencies: curl (or wget), tar, python3 (>= 3.8). NOT auto-installed.
#
# Usage: sh scripts/fetch_lua_tests.sh [--out <dir>]
#   --out  destination (default: tests/fixtures/lua54-tests). Written atomically.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

LUA_VERSION="5.4.7"
ARCHIVE_URL="https://www.lua.org/tests/lua-${LUA_VERSION}-tests.tar.gz"
# The OFFICIAL published SHA-256 of lua-5.4.7-tests.tar.gz (www.lua.org/tests/).
ARCHIVE_SHA256="8a4898ffe4c7613c8009327a0722db7a41ef861d526c77c5b46114e59ebf811e"
SCHEMA_VERSION=1
SELECTION_RULES_VERSION=1

OUT="tests/fixtures/lua54-tests"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        *) echo "fetch_lua_tests: unknown arg: $1" >&2; exit 2 ;;
    esac
done

command -v tar >/dev/null 2>&1 || { echo "fetch_lua_tests: tar not found" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "fetch_lua_tests: python3 not found" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
TARBALL="$WORK/lua-tests.tar.gz"
SRC="$WORK/src"
STAGE="$WORK/stage"
mkdir -p "$SRC" "$STAGE"

echo "fetch_lua_tests: downloading $ARCHIVE_URL ..."
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$TARBALL" "$ARCHIVE_URL"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$TARBALL" "$ARCHIVE_URL"
else
    echo "fetch_lua_tests: neither curl nor wget found" >&2; exit 1
fi

# Verify the official archive SHA-256 BEFORE extracting.
GOT="$(python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$TARBALL")"
if [ "$GOT" != "$ARCHIVE_SHA256" ]; then
    echo "fetch_lua_tests: archive SHA-256 mismatch" >&2
    echo "  got:      $GOT" >&2
    echo "  expected: $ARCHIVE_SHA256" >&2
    exit 1
fi
echo "fetch_lua_tests: archive SHA-256 OK"

tar xzf "$TARBALL" -C "$SRC"

# Select + copy + manifest generation in one deterministic Python pass.
LC_ALL=C SRC="$SRC" STAGE="$STAGE" LUA_VERSION="$LUA_VERSION" ARCHIVE_SHA256="$ARCHIVE_SHA256" \
  SCHEMA_VERSION="$SCHEMA_VERSION" SELECTION_RULES_VERSION="$SELECTION_RULES_VERSION" \
  python3 - <<'PY'
import os, json, hashlib

src = os.environ["SRC"]; stage = os.environ["STAGE"]
lua_version = os.environ["LUA_VERSION"]; archive_sha = os.environ["ARCHIVE_SHA256"]
schema = int(os.environ["SCHEMA_VERSION"]); rules = int(os.environ["SELECTION_RULES_VERSION"])

# The archive extracts to a single top dir lua-<version>-tests/.
top = os.path.join(src, "lua-%s-tests" % lua_version)
if not os.path.isdir(top):
    raise SystemExit("fetch_lua_tests: expected top dir %s" % top)
cases_out = os.path.join(stage, "cases")
os.makedirs(cases_out, exist_ok=True)

selected = []
for dirpath, dirnames, filenames in os.walk(top):
    dirnames.sort()
    for fn in sorted(filenames):
        if not fn.endswith(".lua"):
            continue
        full = os.path.join(dirpath, fn)
        if os.path.islink(full):
            raise SystemExit("symlink in archive (rejected): " + full)
        rel = os.path.relpath(full, top)          # e.g. "math.lua"
        if rel.startswith("/") or ".." in rel.split(os.sep):
            raise SystemExit("path escape: " + rel)
        with open(full, "rb") as f:
            raw = f.read()
        selected.append({"path": rel, "source_hash": hashlib.sha256(raw).hexdigest(), "_raw": raw})

selected.sort(key=lambda c: c["path"].encode("utf-8"))
paths = [c["path"] for c in selected]
if len(set(paths)) != len(paths):
    raise SystemExit("duplicate case paths")

total_bytes = 0
for c in selected:
    dst = os.path.join(cases_out, c["path"])
    real = os.path.realpath(dst)
    if not (real == os.path.realpath(cases_out) or real.startswith(os.path.realpath(cases_out) + os.sep)):
        raise SystemExit("path escapes cases/: " + c["path"])
    os.makedirs(os.path.dirname(dst) or cases_out, exist_ok=True)
    with open(dst, "wb") as f:
        f.write(c["_raw"])
    total_bytes += len(c["_raw"])

manifest = {
    "schema_version": schema,
    "lua_version": lua_version,
    "archive_sha256": archive_sha,
    "selection_rules_version": rules,
    "count": len(selected),
    "cases": [{"path": c["path"], "source_hash": c["source_hash"]} for c in selected],
}
# On-disk bytes ARE the canonical bytes; MANIFEST.sha256 hashes those exact bytes.
manifest_bytes = json.dumps(manifest, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii") + b"\n"
with open(os.path.join(stage, "manifest.json"), "wb") as f:
    f.write(manifest_bytes)
with open(os.path.join(stage, "MANIFEST.sha256"), "w") as f:
    f.write(hashlib.sha256(manifest_bytes).hexdigest() + "\n")

report = {"lua_version": lua_version, "archive_sha256": archive_sha,
          "selected_cases": len(selected), "total_case_bytes": total_bytes,
          "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest()}
with open(os.path.join(stage, "REPORT.json"), "w") as f:
    json.dump(report, f, indent=2, sort_keys=True); f.write("\n")
print(json.dumps(report, indent=2, sort_keys=True))
PY

# The archive bundles no LICENSE; write the Lua MIT license verbatim (www.lua.org/license.html).
cat > "$STAGE/LICENSE" <<'LICENSE'
Lua test suite - Copyright (c) 1994-2024 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
LICENSE

SEL_SUMMARY="$(python3 -c 'import json,sys;r=json.load(open(sys.argv[1]));print("%d .lua files, %d case bytes" % (r["selected_cases"],r["total_case_bytes"]))' "$STAGE/REPORT.json")"
{
    echo "# Official Lua ${LUA_VERSION} test suite (vendored, parser-scoped subset)"
    echo
    echo "- upstream: $ARCHIVE_URL"
    echo "- archive SHA-256: $ARCHIVE_SHA256 (official, verified before extraction)"
    echo "- Lua version: $LUA_VERSION (release-matched to vendor/lua)"
    echo "- fetched: $(date -u +%Y-%m-%dT%H:%M:%SZ) (informational only; NOT part of any hash)"
    echo "- selection-rules version: $SELECTION_RULES_VERSION"
    echo "- selected: $SEL_SUMMARY (regular .lua files only; the execution-suite C libs are excluded)"
    echo "- license: MIT (the Lua license, https://www.lua.org/license.html)."
    echo "- generated by scripts/fetch_lua_tests.sh (never run in CI)."
    echo "- USE: parser-scoped conformance only (load(...,\"t\") oracle vs hull.source.lua)."
    echo "  The suite is NOT executed here; the optional runtime suite is a separate future job."
} > "$STAGE/UPSTREAM.md"

DEST_PARENT="$(dirname "$OUT")"
mkdir -p "$DEST_PARENT"
TMP_DEST="$(mktemp -d "$DEST_PARENT/.lua54.XXXXXX")"
cp -R "$STAGE/." "$TMP_DEST/"
rm -rf "$OUT"
mv "$TMP_DEST" "$OUT"
echo "fetch_lua_tests: wrote $OUT (cases + manifest.json + MANIFEST.sha256 + LICENSE + UPSTREAM.md)"
