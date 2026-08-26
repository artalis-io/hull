#!/usr/bin/env python3
# test_wamrc_artifact.py - fixtures for the wamrc artifact identity/provenance
# core. Proves the pure build_manifest/verify functions: a matching
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
    "image_os": "ubuntu24", "image_version": "20260801.1", "arch": "x86_64",
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


# verify() returns (problems, warnings); most fixtures assert on the problems.
def problems(manifest, loc):
    return wa.verify(manifest, loc)[0]


def warnings(manifest, loc):
    return wa.verify(manifest, loc)[1]


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
check("matching artifact verifies (no problems)", problems(m, local()) == [])

# -- verify: identity mismatches -> rejected --
for field, bad in [("llvm_version", "17.0.0"), ("wamr_rev", "f" * 40),
                   ("patch_hash", "0" * 64), ("wamrc_flags", "-DLLVM_DIR=/other"),
                   ("build_script_hash", "9" * 64), ("cc_version", "cc 12.0.0"),
                   ("cc_path", "/usr/local/bin/cc"), ("image_os", "ubuntu-OTHER"),
                   ("arch", "aarch64")]:
    check("identity mismatch rejected: %s" % field,
          len(problems(m, local(**{field: bad}))) >= 1)

# -- verify: provenance mismatches (different run/attempt/commit) -> rejected --
for field, bad in [("commit_sha", "beefdead" * 5), ("run_id", "99999"),
                   ("run_attempt", "2"), ("producer_job", "some-other-job")]:
    check("provenance mismatch rejected: %s" % field,
          len(problems(m, local(**{field: bad}))) >= 1)

# -- verify: checksum mismatch (corruption) -> rejected --
check("checksum mismatch (corruption) rejected",
      len(problems(m, local(artifact_sha256="d" * 64))) >= 1)
check("MISSING artifact (download failed) rejected",
      len(problems(m, local(artifact_sha256="MISSING"))) >= 1)

# -- verify: unknown / malformed schema -> rejected outright --
check("unknown schema_version rejected", len(problems({**m, "schema_version": 99}, local())) >= 1)
check("no schema_version rejected", len(problems({k: v for k, v in m.items() if k != "schema_version"}, local())) >= 1)
check("non-dict manifest rejected", len(problems([], local())) >= 1)
check("non-dict manifest rejected (str)", len(problems("nope", local())) >= 1)

# -- verify: a manifest missing a field -> rejected --
check("manifest missing a field rejected",
      len(problems({k: v for k, v in m.items() if k != "wamr_rev"}, local())) >= 1)
# -- verify: local missing a field -> rejected --
_partial = dict(GOOD)
del _partial["llvm_version"]
check("local missing a field rejected", len(problems(m, _partial)) >= 1)

# -- hardening: build_manifest rejects hollow / unavailable / empty fields --
for field, bad in [("cc_path", "unknown"), ("cxx_path", "MISSING"),
                   ("cc_version", "unknown"), ("llvm_version", "MISSING"),
                   ("patch_hash", "MISSING"), ("build_script_hash", "MISSING"),
                   ("wamr_rev", "unknown"), ("artifact_sha256", "MISSING"),
                   ("wamrc_flags", ""), ("image_os", "unknown"),
                   ("image_version", "unknown")]:
    try:
        wa.build_manifest(local(**{field: bad}))
        check("build_manifest rejects hollow %s=%r" % (field, bad), False)
    except ValueError:
        check("build_manifest rejects hollow %s=%r" % (field, bad), True)
# None-valued field -> rejected
try:
    wa.build_manifest(local(llvm_version=None))
    check("build_manifest rejects None field", False)
except ValueError:
    check("build_manifest rejects None field", True)

# -- hardening: verify rejects a HOLLOW LOCAL field (a cold consumer that
#    verified WITHOUT first configuring the wamrc toolchain -> cc_path unknown) --
check("verify rejects local cc_path=unknown (cold, no configure)",
      any("cc_path" in p for p in problems(m, local(cc_path="unknown"))))
check("verify rejects local llvm_version=MISSING",
      len(problems(m, local(llvm_version="MISSING"))) >= 1)
check("verify rejects local wamrc_flags='' (empty)",
      len(problems(m, local(wamrc_flags=""))) >= 1)

# -- image_version WARN model (the GitHub runner-image rollout fix) ----------
# Same OS family, DIFFERENT image build number (the EXACT observed rollout pair:
# 20260816.277.1 -> 20260823.283.1). This must VERIFY (no problems) but surface a
# visible WARNING, not reject: the build number is not a toolchain-identity input.
_m_obs = wa.build_manifest(local(image_version="20260816.277.1"))
_obs_probs, _obs_warns = wa.verify(_m_obs, local(image_version="20260823.283.1"))
check("same OS, different image_version -> VERIFIES (no problems)", _obs_probs == [])
check("same OS, different image_version -> emits a WARNING", len(_obs_warns) >= 1)
check("the warning names image_version + both observed build numbers",
      any("image_version" in w and "20260816.277.1" in w and "20260823.283.1" in w
          for w in _obs_warns))
check("identical image_version -> NO warning", warnings(m, local()) == [])

# Different OS FAMILY stays a HARD reject (and is a problem, not merely a warning).
_os_probs, _os_warns = wa.verify(m, local(image_os="ubuntu22"))
check("different image_os (OS family) -> REJECTED as a problem, not a warning",
      len(_os_probs) >= 1 and _os_warns == [])
check("different image_os names image_os in the problem",
      any("image_os" in p for p in _os_probs))

# image_version drift must NOT rescue an otherwise-broken artifact: a checksum
# corruption alongside a drifted image_version is STILL REJECTED.
check("image_version drift + checksum corruption -> still REJECTED",
      any("artifact_sha256" in p
          for p in problems(m, local(image_version="20260823.283.1",
                                     artifact_sha256="d" * 64))))

# Producer / consumer failure remains gate-fatal:
#  - CONSUMER: any problem -> non-empty problems list -> main() returns exit 1
#    (gate-fatal). Proven by every reject fixture above.
#  - PRODUCER: a producer that cannot fully describe its identity FAILS at
#    build_manifest (hollow / None -> ValueError) rather than ship a hollow
#    manifest; a missing/undownloaded artifact (producer/upload-failure surrogate)
#    is a MISSING checksum -> hard reject. Both proven above. The producer JOB
#    failing outright stays gate-fatal via ci.yml `needs:` (the consumer is skipped
#    and the applicability-aware gate treats a non-success producer as failure) -
#    a workflow guarantee this fix does not touch.

# -- cold-verify seam: cmake_compiler_paths reads a CONFIGURE-ONLY cache (no
#    compiled wamrc), proving a consumer can reproduce the producer's compiler
#    identity without building wamrc first (the reliance-flip precondition) --
import tempfile  # noqa: E402
with tempfile.TemporaryDirectory() as d:
    cache = os.path.join(d, "CMakeCache.txt")
    with open(cache, "w") as f:
        f.write("//comment\nCMAKE_C_COMPILER:FILEPATH=/usr/bin/cc\n"
                "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\nOTHER:STRING=x\n")
    cc, cxx = wa.cmake_compiler_paths(cache)
    check("cold cache: cc_path read from configure-only cache", cc == "/usr/bin/cc")
    check("cold cache: cxx_path read from configure-only cache", cxx == "/usr/bin/c++")
    check("cold cache: NO wamrc binary needed to read identity",
          not os.path.exists(os.path.join(d, "wamrc")))
check("absent cache -> MISSING paths (fail closed)",
      wa.cmake_compiler_paths("/no/such/CMakeCache.txt") == ("MISSING", "MISSING"))

print("wamrc_artifact fixtures: %d passed, %d failed" % (_pass, _fail))
sys.exit(1 if _fail else 0)
