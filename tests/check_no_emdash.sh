#!/bin/sh
# check_no_emdash.sh - gate: no em-dash (U+2014) in living first-party prose.
#
# H1 / S5. The house convention is hyphens/colons, never em-dashes, in code
# comments, build files, and docs (see docs/h1_s4_milestone_inventory.md's sibling
# rationale + the S5 record). This gate enforces that in a PRECISELY scoped set of
# living first-party prose and fails on any em-dash there.
#
# SCOPE = every tracked file, MINUS these documented, non-"living-first-party-prose"
# categories (each an exact path or a semantic file-class, never a broad keyword
# grep-out):
#   - vendor/**            third-party vendored source (not first-party)
#   - docs/archive/**      frozen historical design records (immutable bodies)
#   - *.wat *.wasm *.aot   compute test FIXTURES (behavior-bearing data, reviewed
#                          separately; comments there do not affect the artifact)
#   - LICENSE              frozen legal text (AGPL, must stay canonical)
#
# There is NO in-scope allowlist: every behavior-bearing string em-dash was
# rephrased in S5 Area A, so the living scope is expected to be exactly zero.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

# EM DASH byte sequence (U+2014 = e2 80 94), kept out of this file's own prose.
EM=$(printf '\342\200\224')

# Living-source scope: tracked files minus the documented exclusions.
files=$(git ls-files \
  ':(exclude)vendor/**' \
  ':(exclude)docs/archive/**' \
  ':(exclude)*.wat' ':(exclude)*.wasm' ':(exclude)*.aot' \
  ':(exclude)LICENSE')

hits=$(printf '%s\n' "$files" | tr '\n' '\0' | xargs -0 grep -lF "$EM" 2>/dev/null || true)

if [ -n "$hits" ]; then
  echo "check-no-emdash: FAIL - em-dash (U+2014) found in living first-party prose:"
  printf '%s\n' "$hits" | while IFS= read -r f; do
    [ -n "$f" ] || continue
    grep -nF "$EM" "$f" 2>/dev/null | while IFS= read -r ln; do
      echo "  $f:$ln" | cut -c1-140
    done
  done
  echo "Fix: rephrase em-dashes as hyphens/colons (' $EM ' -> ' - '). See H1/S5."
  exit 1
fi
echo "check-no-emdash: OK (no em-dashes in living first-party prose)"
