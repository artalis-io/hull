#!/usr/bin/env python3
# test_nightly_matrix.py - PIN the nightly compat-db engine-version inventory
# (docs/ci_architecture_design.md Appendix F). Supports PostgreSQL 15/16
# and MySQL 8.0 - the versions the e2e harness can actually reach. MySQL 8.4 and
# MariaDB are a PENDING SLOT: 8.4 removed `--default-authentication-plugin`,
# disables mysql_native_password by default, and defaults to caching_sha2_password
# whose full-auth Hull only does over TLS - none set up by the current harness.
#
# This fixture FAILS if nightly.yml's nightly-compat-db image set drifts from the
# approved inventory, so 8.4 / MariaDB cannot be casually re-added without ALSO
# updating this pin - which forces the real compatibility work (TLS-enabled
# caching_sha2_password, version-aware container config, MariaDB's distinct auth).
# Do NOT loosen this pin by forcing legacy mysql_native_password; that tests a
# compatibility escape hatch, not Hull's intended 8.4 path.
#
# No PyYAML dependency (regex over the fixed 2-space job layout, like the other
# scripts/ci checkers).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import os
import re
import sys

WORKFLOW = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), ".github", "workflows", "nightly.yml")

# The APPROVED nightly compat-db engine images. Changing this set is a DELIBERATE
# act that must come WITH the harness work (see the header) - never a casual edit.
APPROVED_COMPAT_DB_IMAGES = frozenset({
    "postgres:15-alpine",
    "postgres:16-alpine",
    "mysql:8.0",
})
# Explicitly NOT supported yet (the pending slot). Their presence is a hard error.
FORBIDDEN_SUBSTRINGS = ("mysql:8.4", "mariadb", "mysql:9", "mysql:8.1", "mysql:8.2", "mysql:8.3")

_pass = 0
_fail = 0


def check(name, cond):
    global _pass, _fail
    if cond:
        _pass += 1
    else:
        _fail += 1
        print("FAIL:", name)


def compat_db_block(path):
    """Return the text of the `nightly-compat-db:` job (up to the next 2-space job
    key or the end of file)."""
    lines = open(path, encoding="utf-8").read().splitlines()
    out, cur = [], False
    for ln in lines:
        m = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", ln)
        if m:
            cur = (m.group(1) == "nightly-compat-db")
            continue
        if cur:
            out.append(ln)
    return "\n".join(out)


block = compat_db_block(WORKFLOW)
check("nightly-compat-db job exists", bool(block.strip()))

images = set(re.findall(r'image:\s*"([^"]+)"', block))
check("compat-db images == approved inventory (PG 15/16 + MySQL 8.0)",
      images == set(APPROVED_COMPAT_DB_IMAGES))
if images != set(APPROVED_COMPAT_DB_IMAGES):
    print("   found:   ", sorted(images))
    print("   approved:", sorted(APPROVED_COMPAT_DB_IMAGES))
    print("   -> re-adding an engine version requires updating THIS pin + the "
          "harness (TLS caching_sha2 / version-aware container), not a casual edit.")

for _bad in FORBIDDEN_SUBSTRINGS:
    check("pending-slot engine absent from executable matrix: %s" % _bad, _bad not in block)

print("nightly_matrix fixtures: %d passed, %d failed" % (_pass, _fail))
sys.exit(1 if _fail else 0)
