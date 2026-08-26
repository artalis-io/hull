#!/bin/sh
# check_no_milestone_narration.sh - gate: no NEW development-milestone narration
# in first-party code / build / config comment prose. H1 / S5.
#
# H1/S4 (+ the S5 completion) stripped development-milestone sequencing ("Phase
# A/B/C/D", "Phase 4.2", "Phase 3d-2", "Slice N", "checkpoint N") from first-party
# COMMENT prose - source, build files, CI config, scripts, tests, examples - and
# recast the enduring rationale to present tense. This gate keeps it that way by
# matching those NARRATION SHAPES narrowly (never a bare "Phase <N>") and failing
# on any hit that is not a reviewed survivor.
#
# SCOPE = every tracked file, MINUS the classes below. Each is an EXACT path or a
# SEMANTIC file-class (not a directory-wide or bare-keyword grep-out), and each is
# documented with WHY it cannot contain the governed prose:
#
#   - vendor/**            third-party vendored source: not first-party; not our
#                          comment prose to govern.
#   - *.md (markdown)      prose DOCUMENTS (design records, guides). A design
#                          record's SUBJECT is the project's design, INCLUDING its
#                          phased history - "Phase 4.3 removed the pre-built flavor
#                          lib" is legitimate content there, not incidental comment
#                          sequencing. The governed category (milestone clutter in
#                          CODE COMMENTS) does not exist in a document by nature.
#                          Markdown is still fully governed by check-no-emdash;
#                          this milestone rule is code-comment-specific. Proven by
#                          the self-test (a shape in a .c bites; the same in a .md
#                          does not).
#   - *.wat *.wasm *.aot   compute test FIXTURES / binary artifacts: behavior data,
#                          not first-party design-comment prose.
#   - this gate + its self-test they contain the narration SHAPES as their
#                          definition / test data; flagging them would be circular.
#
# Additionally, `audit`-bearing lines are a SEMANTIC exception (audit / security
# provenance is always preserved), and the ONE dotted-shape survivor - sbom.c's
# public CLI help "Since Phase 4.3" - is an EXACT-location allowlist entry (from
# docs/h1_s4_milestone_inventory.md). The bare architectural pipeline labels
# (serve.c "Phase 1:".."Phase 11:", sandbox "Phase 1/2") and the jobs public-doc
# changelog are bare "Phase <N>" and match no shape.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

# Development-milestone narration shapes (ERE). Deliberately narrow: a letter
# phase, a dotted minor, a "Nd-" form, or Slice/checkpoint - never a bare
# "Phase <N>" (how the architectural + audit + jobs survivors read).
SHAPES='Phase [A-Z]([^a-z]|$)|Phase [0-9]+\.[0-9]|Phase [0-9]+[a-z]|Phase [0-9]+d-|\bSlice [0-9A-Z]|\bcheckpoint [0-9]'

raw=$(git grep -nE "$SHAPES" -- \
        ':(exclude)vendor/**' \
        ':(exclude)*.md' \
        ':(exclude)*.wat' ':(exclude)*.wasm' ':(exclude)*.aot' \
        ':(exclude)tests/check_no_milestone_narration.sh' \
        ':(exclude)tests/check_no_milestone_narration_selftest.sh' \
        2>/dev/null || true)

# Semantic exception: audit / security provenance lines are always allowed.
raw=$(printf '%s\n' "$raw" | grep -viE 'audit' || true)

# Exact-location survivor allowlist (docs/h1_s4_milestone_inventory.md): the
# sbom.c public CLI help string.
filtered=$(printf '%s\n' "$raw" | grep -v -e 'src/hull/commands/sbom.c:.*Since Phase 4\.3' || true)
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
echo "check-no-milestone-narration: OK (no new milestone narration in first-party code/build prose)"
