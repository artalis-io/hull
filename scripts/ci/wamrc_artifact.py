#!/usr/bin/env python3
# wamrc_artifact.py - the wamrc producer/consumer artifact identity + provenance
# core for Slice 5A (docs/ci_architecture_design.md Appendix D).
#
# The PRODUCER builds `wamrc` once and emits a metadata manifest (identity +
# same-run provenance + sha256). Each CONSUMER downloads the run-scoped artifact,
# recomputes its OWN identity/provenance, and verifies it against the manifest.
# ANY mismatch is a hard failure (the consumer exits non-zero and FAILS its job -
# it does NOT silently rebuild from source; that would mask broken producer/upload
# wiring - Appendix D.1.3, amendment 1).
#
# Pure functions (build_manifest / verify) are fixture-tested in
# test_wamrc_artifact.py; gather_local() does the environment probing.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import argparse
import glob
import hashlib
import json
import os
import re
import subprocess
import sys

SCHEMA_VERSION = 1

# The reuse-identity inputs (a change to any -> a different key -> no reuse).
IDENTITY_FIELDS = [
    "runner_image",        # ImageOS-ImageVersion (not just "ubuntu-24.04")
    "arch",                # uname -m
    "cc_path", "cc_version",     # the actual CMake C compiler path + version
    "cxx_path", "cxx_version",   # the actual CMake C++ compiler path + version
    "llvm_version",        # llvm-config-18 --version
    "wamr_rev",            # vendor/wamr submodule HEAD
    "patch_hash",          # sha256 of patches/wamr/*.patch
    "wamrc_flags",         # WAMRC_CMAKE_FLAGS
    "build_script_hash",   # sha256 of the wamrc make rule region + ci_ensure_wamrc.sh
]
# Same-run provenance (proves the artifact belongs to THIS run).
PROVENANCE_FIELDS = ["commit_sha", "run_id", "run_attempt", "producer_job"]
# The artifact digest (checksum integrity).
CHECKSUM_FIELD = "artifact_sha256"

ALL_FIELDS = IDENTITY_FIELDS + PROVENANCE_FIELDS + [CHECKSUM_FIELD]


def build_manifest(fields):
    """Wrap gathered fields into a schema-versioned manifest. Raises if any
    required field is absent (a producer that cannot describe itself must fail)."""
    missing = [k for k in ALL_FIELDS if k not in fields]
    if missing:
        raise ValueError("manifest missing required fields: %s" % ", ".join(missing))
    m = {"schema_version": SCHEMA_VERSION}
    for k in ALL_FIELDS:
        m[k] = str(fields[k])
    return m


def verify(manifest, local):
    """Return a list of problems (empty => the artifact is trusted). `manifest` is
    the producer's recorded metadata; `local` is the consumer's freshly-recomputed
    identity/provenance (including the sha256 of the DOWNLOADED wamrc). An unknown
    schema, a missing field, or ANY field mismatch is a problem -> the caller fails
    the job. Fail closed: a non-dict manifest, or a schema it does not understand,
    is rejected outright."""
    if not isinstance(manifest, dict):
        return ["manifest is not a JSON object"]
    if manifest.get("schema_version") != SCHEMA_VERSION:
        return ["unknown manifest schema_version %r (expected %d)"
                % (manifest.get("schema_version"), SCHEMA_VERSION)]
    problems = []
    for k in ALL_FIELDS:
        if k not in manifest:
            problems.append("manifest missing field %s" % k)
        if k not in local:
            problems.append("local missing field %s" % k)
    for k in ALL_FIELDS:
        if k in manifest and k in local and str(manifest[k]) != str(local[k]):
            problems.append("%s mismatch: manifest=%r local=%r" % (k, manifest[k], local[k]))
    return problems


# -- environment probing (impure; not unit-tested, kept thin) -----------------

def _run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()
    except Exception:
        return ""


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def gather_local(wamrc_path, producer_job, root):
    """Probe the current environment for every identity + provenance field, and
    compute the sha256 of `wamrc_path` (the local or downloaded wamrc)."""
    env = os.environ
    runner_image = "%s-%s" % (env.get("ImageOS", "unknown"), env.get("ImageVersion", "unknown"))
    llvm_version = _run(["llvm-config-18", "--version"]) or "unknown"
    wamr_rev = _run(["git", "-C", os.path.join(root, "vendor", "wamr"), "rev-parse", "HEAD"]) or "unknown"

    ph = hashlib.sha256()
    for p in sorted(glob.glob(os.path.join(root, "patches", "wamr", "*.patch"))):
        with open(p, "rb") as f:
            ph.update(f.read())
    patch_hash = ph.hexdigest()

    bh = hashlib.sha256()
    try:
        mk = open(os.path.join(root, "Makefile"), encoding="utf-8").read()
        m = re.search(r"(?ms)^wamrc:.*?wamrc built.*?$", mk)
        bh.update((m.group(0) if m else "").encode("utf-8"))
    except Exception:
        pass
    try:
        with open(os.path.join(root, "tests", "ci_ensure_wamrc.sh"), "rb") as f:
            bh.update(f.read())
    except Exception:
        pass
    build_script_hash = bh.hexdigest()

    cc_path = cxx_path = "unknown"
    cache = os.path.join(root, "build", "wamrc-build", "CMakeCache.txt")
    if os.path.exists(cache):
        for ln in open(cache, encoding="utf-8", errors="replace"):
            if ln.startswith("CMAKE_C_COMPILER:"):
                cc_path = ln.split("=", 1)[1].strip()
            elif ln.startswith("CMAKE_CXX_COMPILER:"):
                cxx_path = ln.split("=", 1)[1].strip()
    cc_version = (_run([cc_path, "--version"]).splitlines() or ["unknown"])[0] if cc_path != "unknown" else "unknown"
    cxx_version = (_run([cxx_path, "--version"]).splitlines() or ["unknown"])[0] if cxx_path != "unknown" else "unknown"

    artifact_sha256 = _sha256_file(wamrc_path) if os.path.exists(wamrc_path) else "MISSING"

    return {
        "runner_image": runner_image, "arch": _run(["uname", "-m"]) or "unknown",
        "cc_path": cc_path, "cc_version": cc_version,
        "cxx_path": cxx_path, "cxx_version": cxx_version,
        "llvm_version": llvm_version, "wamr_rev": wamr_rev, "patch_hash": patch_hash,
        "wamrc_flags": env.get("WAMRC_CMAKE_FLAGS", ""),
        "build_script_hash": build_script_hash,
        "commit_sha": env.get("GITHUB_SHA", "unknown"),
        "run_id": env.get("GITHUB_RUN_ID", "unknown"),
        "run_attempt": env.get("GITHUB_RUN_ATTEMPT", "unknown"),
        "producer_job": producer_job,
        "artifact_sha256": artifact_sha256,
    }


def _root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main(argv):
    ap = argparse.ArgumentParser(description="wamrc artifact manifest / verify (Slice 5A).")
    sub = ap.add_subparsers(dest="cmd", required=True)
    mp = sub.add_parser("manifest", help="emit the producer manifest JSON to stdout.")
    mp.add_argument("--wamrc", required=True)
    mp.add_argument("--producer", required=True)
    vp = sub.add_parser("verify", help="verify a downloaded artifact against this run.")
    vp.add_argument("--manifest", required=True)
    vp.add_argument("--wamrc", required=True)
    vp.add_argument("--producer", required=True)
    args = ap.parse_args(argv)
    root = _root()

    if args.cmd == "manifest":
        local = gather_local(args.wamrc, args.producer, root)
        print(json.dumps(build_manifest(local), sort_keys=True, indent=2))
        return 0

    # verify
    try:
        with open(args.manifest, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except Exception as e:
        print("wamrc-verify: cannot read manifest (%s) -> FAIL" % e)
        return 1
    local = gather_local(args.wamrc, args.producer, root)
    problems = verify(manifest, local)
    if problems:
        for p in problems:
            print("  FAIL:", p)
        print("wamrc-verify: artifact REJECTED (%d problem(s)); NOT rebuilding - failing the job."
              % len(problems))
        return 1
    print("wamrc-verify: artifact identity + provenance + checksum verified.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
