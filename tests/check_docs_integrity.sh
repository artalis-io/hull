#!/bin/sh
# tests/check_docs_integrity.sh - permanent documentation-integrity gate.
#
# Protects the docs/ reorganization (active / invariants / historical) from
# silent rot. Five checks, each fatal:
#
#   1. CATALOG      every top-level docs/*.md is catalogued in docs/README.md
#   2. LINKS        every relative Markdown link (in docs/** + the root
#                   README/CONTRIBUTING/SECURITY/CLAUDE/AGENTS/BOOTSTRAP)
#                   resolves to an existing file
#   3. ARCHIVE      every file under docs/archive/ is inventoried by
#                   docs/archive/README.md (archived docs are classified as
#                   historical, never floating as pseudo-active specs)
#   4. SOURCE-REFS  every first-party `docs/....md` reference in code / docs /
#                   instructions resolves (illustrative examples allowlisted)
#   5. RESURRECTION moved/archived historical docs cannot silently reappear at
#                   their old top-level docs/ path
#
# POSIX sh. Run from anywhere; it locates the repo root from its own path.
# Wired into `make lint` (target: check-docs-integrity); the negative self-test
# tests/check_docs_integrity_selftest.sh proves it bites.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

# Repo root = parent of this script's tests/ dir.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

FAIL=0
err() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL + 1)); }
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }

# Illustrative / synthetic docs paths used as literals in code (CI classifier
# tests, path-normalization examples) - NOT real doc links. Keep this list tight.
is_allowlisted_ref() {
    case "$1" in
        # synthetic inputs in CI-classifier / path tests
        docs/x.md|docs/a.md|docs/b.md|docs/moved.md|docs/new.md|docs/only.md) return 0 ;;
        # the literal pattern "docs/....md" appears in prose describing this gate
        docs/....md) return 0 ;;
        # planned (not-yet-written) artifact named in the DEFERRED reporting-IR design
        docs/reporting.md) return 0 ;;
        *) return 1 ;;
    esac
}

# Historical docs that were physically relocated. Old top-level path must stay
# gone; the value is where it now lives (for the message).
MOVED_PATHS="
docs/cachelib_spike.md>docs/archive/design_records/cachelib_spike.md
docs/kvmem_design.md>docs/archive/design_records/kvmem_design.md
docs/kvmem_negative_result.md>docs/archive/design_records/kvmem_negative_result.md
docs/memstore_lru_plan.md>docs/archive/design_records/memstore_lru_plan.md
docs/jobs_wasm_replay_spike.md>docs/archive/design_records/jobs_wasm_replay_spike.md
docs/api_review.md>docs/archive/api_review.md
docs/ASSESSMENT.md>docs/archive/ASSESSMENT.md
docs/audit_2026_05_15.md>docs/archive/audits/audit_2026_05_15.md
docs/audit_2026_05_15_phase6.md>docs/archive/audits/audit_2026_05_15_phase6.md
docs/audit_2026_05_15_phase6_reaudit.md>docs/archive/audits/audit_2026_05_15_phase6_reaudit.md
"

# ── 1. CATALOG: every top-level docs/*.md is linked from docs/README.md ────────
check_catalog() {
    linked=$(grep -oE '\]\([^)]+\)' docs/README.md \
             | sed -E 's/\]\(([^)#]+)(#[^)]*)?\)/\1/' \
             | while IFS= read -r t; do basename "$t"; done | sort -u)
    for f in docs/*.md; do
        b=$(basename "$f")
        [ "$b" = "README.md" ] && continue
        if printf '%s\n' "$linked" | grep -qxF "$b"; then :; else
            err "1/CATALOG: docs/$b is not catalogued in docs/README.md"
        fi
    done
    [ "$FAIL" -eq 0 ] && ok "1/CATALOG: every top-level docs/*.md is in docs/README.md"
}

# ── 2. LINKS: every relative Markdown link resolves ───────────────────────────
# Scope: ACTIVE docs (top-level docs/*.md) + every index README (incl. archive
# indexes) + the root policy/readme files. Frozen archived RECORD bodies under
# docs/archive/*/ are NOT deep-link-audited - they are historical snapshots whose
# cross-references reflect the state at the time, not the present tree.
check_links() {
    before=$FAIL
    files=$( { ls docs/*.md 2>/dev/null; find docs -name 'README.md';
               printf '%s\n' README.md CONTRIBUTING.md SECURITY.md \
                             CLAUDE.md AGENTS.md BOOTSTRAP.md; } | sort -u)
    printf '%s\n' "$files" | while IFS= read -r src; do
        [ -f "$src" ] || continue
        dir=$(dirname "$src")
        grep -oE '\]\([^)]+\)' "$src" \
        | sed -E 's/\]\(([^)]+)\)/\1/' \
        | while IFS= read -r target; do
            case "$target" in
                http://*|https://*|mailto:*|'#'*|'') continue ;;
            esac
            # strip a #anchor suffix
            path=${target%%#*}
            [ -z "$path" ] && continue
            # resolve relative to the source file's directory (handles ../)
            if ( cd "$dir" 2>/dev/null && [ -e "$path" ] ); then :; else
                printf 'BROKEN %s -> %s\n' "$src" "$target"
            fi
          done
    done > /tmp/hull_docs_links.$$ 2>/dev/null || true
    if [ -s /tmp/hull_docs_links.$$ ]; then
        while IFS= read -r line; do err "2/LINKS: ${line#BROKEN }"; done < /tmp/hull_docs_links.$$
    fi
    rm -f /tmp/hull_docs_links.$$
    [ "$FAIL" -eq "$before" ] && ok "2/LINKS: all relative Markdown links resolve"
}

# ── 3. ARCHIVE: every archived doc is inventoried by docs/archive/README.md ────
check_archive() {
    before=$FAIL
    [ -f docs/archive/README.md ] || { err "3/ARCHIVE: docs/archive/README.md missing"; return; }
    inv=docs/archive/README.md
    find docs/archive -name '*.md' | while IFS= read -r f; do
        b=$(basename "$f")
        [ "$b" = "README.md" ] && continue
        # the inventory must mention the file basename somewhere
        if grep -qF "$b" "$inv"; then :; else
            printf 'UNLISTED %s\n' "$f"
        fi
    done > /tmp/hull_docs_arch.$$ 2>/dev/null || true
    if [ -s /tmp/hull_docs_arch.$$ ]; then
        while IFS= read -r line; do
            err "3/ARCHIVE: ${line#UNLISTED } not inventoried in docs/archive/README.md"
        done < /tmp/hull_docs_arch.$$
    fi
    rm -f /tmp/hull_docs_arch.$$
    [ "$FAIL" -eq "$before" ] && ok "3/ARCHIVE: every archived doc is inventoried"
}

# ── 4. SOURCE-REFS: first-party docs/....md references resolve ─────────────────
check_source_refs() {
    before=$FAIL
    # First-party trees only; exclude vendored / generated / build dirs and this
    # gate's own scripts (their MOVED_PATHS / allowlist literals are not doc links).
    # docs/archive/ record BODIES are frozen historical snapshots - their prose
    # `docs/X.md` mentions reflect the state at the time, so they are excluded from
    # the ACTIVE source-reference check (mirrors the LINKS scope).
    refs=$(grep -rhoE 'docs/[A-Za-z0-9_./-]+\.md' \
             --exclude-dir=.playwright --exclude-dir=node_modules \
             --exclude-dir=vendor --exclude-dir=build --exclude-dir=.git \
             --exclude-dir=archive \
             --exclude=check_docs_integrity.sh \
             --exclude=check_docs_integrity_selftest.sh \
             src include tests scripts CLAUDE.md AGENTS.md README.md \
             CONTRIBUTING.md SECURITY.md docs 2>/dev/null \
           | sed 's#docs//#docs/#' | sort -u)
    printf '%s\n' "$refs" | while IFS= read -r ref; do
        [ -z "$ref" ] && continue
        is_allowlisted_ref "$ref" && continue
        [ -f "$ref" ] || printf 'DANGLING %s\n' "$ref"
    done > /tmp/hull_docs_refs.$$ 2>/dev/null || true
    if [ -s /tmp/hull_docs_refs.$$ ]; then
        while IFS= read -r line; do
            err "4/SOURCE-REFS: ${line#DANGLING } referenced in code/docs but does not exist"
        done < /tmp/hull_docs_refs.$$
    fi
    rm -f /tmp/hull_docs_refs.$$
    [ "$FAIL" -eq "$before" ] && ok "4/SOURCE-REFS: every first-party docs/*.md reference resolves"
}

# ── 5. RESURRECTION: moved historical docs stay gone from docs/ top level ──────
check_resurrection() {
    before=$FAIL
    printf '%s\n' "$MOVED_PATHS" | while IFS= read -r pair; do
        [ -z "$pair" ] && continue
        old=${pair%%>*}; new=${pair#*>}
        if [ -e "$old" ]; then
            printf 'REAPPEARED %s (belongs at %s)\n' "$old" "$new"
        elif [ ! -e "$new" ]; then
            printf 'MISSINGNEW %s (moved target %s not found)\n' "$old" "$new"
        fi
    done > /tmp/hull_docs_res.$$ 2>/dev/null || true
    if [ -s /tmp/hull_docs_res.$$ ]; then
        while IFS= read -r line; do
            case "$line" in
                REAPPEARED*) err "5/RESURRECTION: ${line#REAPPEARED }" ;;
                MISSINGNEW*) err "5/RESURRECTION: ${line#MISSINGNEW }" ;;
            esac
        done < /tmp/hull_docs_res.$$
    fi
    rm -f /tmp/hull_docs_res.$$
    [ "$FAIL" -eq "$before" ] && ok "5/RESURRECTION: moved historical docs have not reappeared"
}

echo "docs-integrity gate:"
check_catalog
check_links
check_archive
check_source_refs
check_resurrection

if [ "$FAIL" -ne 0 ]; then
    printf '\n\033[31m%d documentation-integrity violation(s).\033[0m\n' "$FAIL"
    printf 'Fix the doc/link, or update docs/README.md / docs/archive/README.md.\n'
    exit 1
fi
printf '\n\033[32mAll documentation-integrity checks passed.\033[0m\n'
exit 0
