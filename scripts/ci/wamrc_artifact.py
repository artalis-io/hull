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

# Sentinels a probe emits when a value could not be determined. In this CI profile
# EVERY field must be concrete: a manifest carrying any of these (or an empty
# string, e.g. an empty WAMRC_CMAKE_FLAGS, or an absent Makefile-rule / script /
# patch input) is REJECTED at creation rather than shipped with a hollow identity.
INVALID_VALUES = frozenset({"", "unknown", "MISSING", "none", "None"})


def _invalid_fields(fields):
    """Return the fields whose value is absent/unavailable/hollow."""
    bad = []
    for k in ALL_FIELDS:
        v = fields.get(k, None)
        if v is None or str(v).strip() in INVALID_VALUES:
            bad.append(k)
    return bad


def build_manifest(fields):
    """Wrap gathered fields into a schema-versioned manifest. Raises if any
    required field is absent OR unavailable/hollow (a producer that cannot fully
    describe its toolchain identity must FAIL, not ship a manifest with `unknown`
    / `MISSING` / empty fields that a consumer could never reproduce)."""
    bad = _invalid_fields(fields)
    if bad:
        raise ValueError("cannot build manifest; unavailable/invalid fields: %s"
                         % ", ".join("%s=%r" % (k, fields.get(k)) for k in bad))
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
    # The consumer's OWN identity must be fully determined; a hollow local field
    # (e.g. `cc_path=unknown` because a cold consumer verified WITHOUT first
    # configuring the wamrc toolchain) is a verification failure, not a silent
    # mismatch.
    for k in _invalid_fields(local):
        problems.append("local field %s is unavailable/invalid: %r" % (k, local.get(k)))
    for k in ALL_FIELDS:
        if k in manifest and k in local and str(manifest[k]) != str(local[k]):
            problems.append("%s mismatch: manifest=%r local=%r" % (k, manifest[k], local[k]))
    return problems


# -- environment probing (impure; kept thin) ---------------------------------

def _run(cmd):
    """Run a command; return its stripped stdout on SUCCESS (exit 0), or None on a
    nonzero exit or spawn failure. A nonzero command is a REJECTED probe (its field
    becomes unavailable and the manifest fails), NOT an accepted empty string."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
    except Exception:
        return None
    if r.returncode != 0:
        return None
    return r.stdout.strip()


def _first_line(cmd):
    out = _run(cmd)
    if not out:
        return None
    lines = out.splitlines()
    return lines[0] if lines else None


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def cmake_compiler_paths(cache_path):
    """Read (cc_path, cxx_path) from a CMakeCache.txt. This is the COLD-verify seam:
    it works off a configure-only cache (no compiled wamrc), so a consumer can
    reproduce the producer's compiler identity without building wamrc. Returns
    ('MISSING', 'MISSING') if the cache is absent, and leaves a compiler 'MISSING'
    if its line is not present. Pure file parse (fixture-tested)."""
    cc = cxx = "MISSING"
    if not os.path.exists(cache_path):
        return cc, cxx
    for ln in open(cache_path, encoding="utf-8", errors="replace"):
        if ln.startswith("CMAKE_C_COMPILER:"):
            cc = ln.split("=", 1)[1].strip() or "MISSING"
        elif ln.startswith("CMAKE_CXX_COMPILER:"):
            cxx = ln.split("=", 1)[1].strip() or "MISSING"
    return cc, cxx


def gather_local(wamrc_path, producer_job, root):
    """Probe the current environment for every identity + provenance field, and
    compute the sha256 of `wamrc_path`. Every probe that cannot determine a value
    yields a `MISSING`/`unknown` sentinel so build_manifest / verify fail closed
    (an absent patch set, an unmatched Makefile rule, a missing script, an empty
    WAMRC_CMAKE_FLAGS, or a compiler the CMakeCache did not record)."""
    env = os.environ
    runner_image = "%s-%s" % (env.get("ImageOS", "unknown"), env.get("ImageVersion", "unknown"))

    ph = hashlib.sha256()
    patches = sorted(glob.glob(os.path.join(root, "patches", "wamr", "*.patch")))
    if patches:
        for p in patches:
            with open(p, "rb") as f:
                ph.update(f.read())
        patch_hash = ph.hexdigest()
    else:
        patch_hash = "MISSING"        # no expected patch inputs -> reject

    # build-script hash: the wamrc-configure + wamrc make-rule region AND
    # ci_ensure_wamrc.sh. A missing rule match or missing script -> MISSING.
    bh = hashlib.sha256()
    rule_ok = script_ok = False
    try:
        mk = open(os.path.join(root, "Makefile"), encoding="utf-8").read()
        m = re.search(r"(?ms)^wamrc-configure:.*?wamrc built.*?$", mk)
        if m:
            bh.update(m.group(0).encode("utf-8"))
            rule_ok = True
    except Exception:
        pass
    try:
        with open(os.path.join(root, "tests", "ci_ensure_wamrc.sh"), "rb") as f:
            bh.update(f.read())
        script_ok = True
    except Exception:
        pass
    build_script_hash = bh.hexdigest() if (rule_ok and script_ok) else "MISSING"

    cache = os.path.join(root, "build", "wamrc-build", "CMakeCache.txt")
    cc_path, cxx_path = cmake_compiler_paths(cache)
    cc_version = _first_line([cc_path, "--version"]) if cc_path not in INVALID_VALUES else None
    cxx_version = _first_line([cxx_path, "--version"]) if cxx_path not in INVALID_VALUES else None

    return {
        "runner_image": runner_image,
        "arch": _run(["uname", "-m"]) or "unknown",
        "cc_path": cc_path, "cc_version": cc_version or "unknown",
        "cxx_path": cxx_path, "cxx_version": cxx_version or "unknown",
        "llvm_version": _run(["llvm-config-18", "--version"]) or "unknown",
        "wamr_rev": _run(["git", "-C", os.path.join(root, "vendor", "wamr"), "rev-parse", "HEAD"]) or "unknown",
        "patch_hash": patch_hash,
        "wamrc_flags": env.get("WAMRC_CMAKE_FLAGS", ""),   # empty -> invalid (rejected)
        "build_script_hash": build_script_hash,
        "commit_sha": env.get("GITHUB_SHA", "unknown"),
        "run_id": env.get("GITHUB_RUN_ID", "unknown"),
        "run_attempt": env.get("GITHUB_RUN_ATTEMPT", "unknown"),
        "producer_job": producer_job,
        "artifact_sha256": _sha256_file(wamrc_path) if os.path.exists(wamrc_path) else "MISSING",
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
