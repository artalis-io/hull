#!/bin/sh
# check_no_milestone_narration.sh - gate: no NEW development-milestone narration
# in the S4-reviewed code + build surface. H1 / S5.
#
# H1/S4 stripped development-milestone sequencing ("Phase A/B/C/D", "Phase 4.2",
# "Phase 3d-2", "Slice N", "checkpoint N", dated milestone tags) from first-party
# code comments and build files, recasting the enduring rationale to present
# tense. This gate keeps it that way by matching those NARRATION SHAPES - narrowly,
# never a bare "Phase"/"Slice" keyword - and failing on any hit that is not an
# exact reviewed survivor from docs/h1_s4_milestone_inventory.md.
#
# SCOPE = the surface S4 reviewed: src/ include/ stdlib/ Makefile mk/ templates/.
# (tests/, examples/, and docs/ are intentionally NOT covered - S4 did not review
# their design-phase labels, and docs legitimately narrate design history. Those
# are a separate, later concern; do not fold them in via a broad exclusion here.)
#
# SURVIVORS kept (from the inventory), and why they do not trip this gate:
#   - Architectural pipeline labels: serve.c "Phase 1:".."Phase 11:" and
#     sandbox.c/.h "Phase 1 (pledge) / Phase 2" are BARE "Phase <N>" and do not
#     match any narration shape below (which require a letter, a dotted minor, a
#     "3d-" form, "Slice", or "checkpoint").
#   - Audit / security provenance: "Phase 6 audit L-#/M-#", "Phase 1 audit
#     (date)" are bare "Phase <N>" + text; also semantically excluded (any line
#     mentioning "audit") so provenance is never at risk.
#   - Public API doc changelog: jobs.js/jobs.lua "..., Phase 1)" is bare and does
#     not match.
#   - Public CLI help text: sbom.c "(Since Phase 4.3 ...)" DOES match the dotted
#     shape and is the ONE exact-location allowlisted survivor below.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

# Development-milestone narration shapes (ERE). Deliberately narrow: they require
# a letter phase, a dotted minor, a "3d-" form, or Slice/checkpoint - never a bare
# "Phase <N>" (which is how the architectural + audit + jobs survivors read).
SHAPES='Phase [A-Z]([^a-z]|$)|Phase [0-9]+\.[0-9]|Phase [0-9]+[a-z]|Phase [0-9]+d-|\bSlice [0-9A-Z]|\bcheckpoint [0-9]'

raw=$(git grep -nE "$SHAPES" -- \
        'src/' 'include/' 'stdlib/' 'Makefile' 'mk/' 'templates/' 2>/dev/null || true)

# Semantic exception: audit/security provenance lines are always allowed.
raw=$(printf '%s\n' "$raw" | grep -viE 'audit' || true)

# Exact-location survivor allowlist (from docs/h1_s4_milestone_inventory.md).
# One entry: the sbom.c public CLI help string. Matched by exact file + text.
filtered=$(printf '%s\n' "$raw" | grep -v \
    -e 'src/hull/commands/sbom.c:.*Since Phase 4\.3' || true)

filtered=$(printf '%s\n' "$filtered" | sed '/^$/d')

if [ -n "$filtered" ]; then
  echo "check-no-milestone-narration: FAIL - new development-milestone narration"
  echo "  (recast to present tense, or - if a genuine reviewed survivor - add an"
  echo "   EXACT-location entry to this gate + docs/h1_s4_milestone_inventory.md):"
  printf '%s\n' "$filtered" | while IFS= read -r ln; do
    [ -n "$ln" ] && echo "  $ln" | cut -c1-140
  done
  exit 1
fi
echo "check-no-milestone-narration: OK (no new milestone narration in the S4 scope)"
