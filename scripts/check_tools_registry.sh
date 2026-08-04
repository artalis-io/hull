#!/bin/sh
# T4d (docs/build_arc_audit.md): fail if the side-load tool REGISTRY in
# src/hull/tools_install.c drifts from the release workflow that must publish each
# tool's asset. The registry is the CONSUMER-side truth (install / lookup / list);
# .github/workflows/release.yml is the PRODUCER. A new registry row with no
# matching release asset would `hull tools install`-fail at runtime, and the
# post-release tests/release_smoke.sh would be the first to notice - this catches
# it per push instead.
#
# For each REGISTRY `.name`, assert `hull-<name>` appears in release.yml (as a
# binary `hull-<name>-<platform>` or a bundle `hull-<name>[-<platform>].tar`).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
TI=src/hull/tools_install.c
RY=.github/workflows/release.yml

names=$(grep -E '^[[:space:]]*\.name[[:space:]]*=[[:space:]]*"[a-z0-9._-]+"' "$TI" \
        | sed -E 's/.*"([a-z0-9._-]+)".*/\1/' | sort -u)
[ -n "$names" ] || { echo "ERROR: no tool names parsed from $TI" >&2; exit 1; }

miss=0
for n in $names; do
  if ! grep -q "hull-$n" "$RY"; then
    echo "ERROR: tool '$n' (tools_install.c REGISTRY) has no 'hull-$n' asset in $RY" >&2
    echo "  -> publish hull-$n[-<platform>][.tar] in the release workflow (build job" >&2
    echo "     + the flatten / sha256 / attest / publish lists)" >&2
    miss=1
  fi
done
[ "$miss" -eq 0 ] || exit 1

echo "check-tools-registry: OK ($(echo $names | tr '\n' ' '))"
