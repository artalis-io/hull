#!/usr/bin/env python3
# test_wamrc_artifact.py - fixtures for the wamrc artifact identity/provenance
# core (Slice 5A). Proves the pure build_manifest/verify functions: a matching
# artifact verifies; ANY identity, provenance, checksum, arch, schema, or
# missing-field defect is rejected (-> the consumer fails, never rebuilds).
# Run: python3 scripts/ci/test_wamrc_artifact.py
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wamrc_artifact as wa  # noqa: E402

_pass = 0
_fail = 0


def check(name, cond):
    global _pass, _fail
    if cond:
        _pass += 1
    else:
        _fail += 1
        print("FAIL:", name)


# A complete, self-consistent set of gathered fields.
GOOD = {
    "runner_image": "ubuntu24-20260801.1", "arch": "x86_64",
    "cc_path": "/usr/bin/cc", "cc_version": "cc (Ubuntu) 13.2.0",
    "cxx_path": "/usr/bin/c++", "cxx_version": "c++ (Ubuntu) 13.2.0",
    "llvm_version": "18.1.3", "wamr_rev": "c3a78cd159e59c86ac4543308bd676ff78d30a93",
    "patch_hash": "a" * 64, "wamrc_flags": "-DLLVM_DIR=/usr/lib/llvm-18/cmake",
    "build_script_hash": "b" * 64,
    "commit_sha": "deadbeef" * 5, "run_id": "12345", "run_attempt": "1",
    "producer_job": "wamrc-x86_64", "artifact_sha256": "c" * 64,
}


def local(**overrides):
    d = dict(GOOD)
    d.update(overrides)
    return d


# -- build_manifest --
m = wa.build_manifest(GOOD)
check("manifest carries schema_version", m["schema_version"] == wa.SCHEMA_VERSION)
check("manifest has every field", all(k in m for k in wa.ALL_FIELDS))
check("manifest stringifies values", all(isinstance(m[k], str) for k in wa.ALL_FIELDS))
try:
    wa.build_manifest({"arch": "x86_64"})
    check("build_manifest rejects missing fields", False)
except ValueError:
    check("build_manifest rejects missing fields", True)

# -- verify: the happy path --
check("matching artifact verifies (no problems)", wa.verify(m, local()) == [])

# -- verify: identity mismatches -> rejected --
for field, bad in [("llvm_version", "17.0.0"), ("wamr_rev", "f" * 40),
                   ("patch_hash", "0" * 64), ("wamrc_flags", "-DLLVM_DIR=/other"),
                   ("build_script_hash", "9" * 64), ("cc_version", "cc 12.0.0"),
                   ("cc_path", "/usr/local/bin/cc"), ("runner_image", "ubuntu24-OTHER"),
                   ("arch", "aarch64")]:
    check("identity mismatch rejected: %s" % field,
          len(wa.verify(m, local(**{field: bad}))) >= 1)

# -- verify: provenance mismatches (different run/attempt/commit) -> rejected --
for field, bad in [("commit_sha", "beefdead" * 5), ("run_id", "99999"),
                   ("run_attempt", "2"), ("producer_job", "some-other-job")]:
    check("provenance mismatch rejected: %s" % field,
          len(wa.verify(m, local(**{field: bad}))) >= 1)

# -- verify: checksum mismatch (corruption) -> rejected --
check("checksum mismatch (corruption) rejected",
      len(wa.verify(m, local(artifact_sha256="d" * 64))) >= 1)
check("MISSING artifact (download failed) rejected",
      len(wa.verify(m, local(artifact_sha256="MISSING"))) >= 1)

# -- verify: unknown / malformed schema -> rejected outright --
check("unknown schema_version rejected", len(wa.verify({**m, "schema_version": 99}, local())) >= 1)
check("no schema_version rejected", len(wa.verify({k: v for k, v in m.items() if k != "schema_version"}, local())) >= 1)
check("non-dict manifest rejected", len(wa.verify([], local())) >= 1)
check("non-dict manifest rejected (str)", len(wa.verify("nope", local())) >= 1)

# -- verify: a manifest missing a field -> rejected --
check("manifest missing a field rejected",
      len(wa.verify({k: v for k, v in m.items() if k != "wamr_rev"}, local())) >= 1)
# -- verify: local missing a field -> rejected --
_partial = dict(GOOD)
del _partial["llvm_version"]
check("local missing a field rejected", len(wa.verify(m, _partial)) >= 1)

print("wamrc_artifact fixtures: %d passed, %d failed" % (_pass, _fail))
sys.exit(1 if _fail else 0)
