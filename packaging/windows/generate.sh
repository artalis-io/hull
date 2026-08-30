#!/bin/sh
# generate.sh - generate the repository-owned Winget + Scoop package metadata for
# Hull on Windows from a release tag + the release's hull.sha256 manifest.
#
# The manifests pin the immutable official release URL and the exact hull-cosmo
# SHA-256. This generator is the single source of the pinned hash (never
# hand-maintained), and it FAILS CLOSED if release metadata or hashes disagree:
#   - the hull.sha256 manifest must be present and readable;
#   - it must contain exactly one hull-cosmo entry (a missing or duplicate entry
#     is an error);
#   - the entry must be a 64-hex SHA-256.
#
# Usage:
#   generate.sh --tag <vX.Y.Z> [--sha256-file <path>] [--out <dir>]
#   generate.sh --tag <vX.Y.Z> [--sha256-file <path>] --check
#
#   --sha256-file  read hull.sha256 from a local file instead of downloading it
#                  from the release (the download uses HTTPS).
#   --out          output directory (default: the packaging/windows dir).
#   --check        regenerate into a temp dir and diff against the committed
#                  files; exit non-zero if they differ (a drift gate for CI).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

unset CDPATH 2>/dev/null || true
SELF_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TEMPLATES="$SELF_DIR/templates"
REPO="artalis-io/hull"

TAG=""
SHA_FILE=""
OUT="$SELF_DIR"
CHECK=0

die() { echo "generate.sh: error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --tag) TAG="${2:-}"; shift 2 ;;
        --tag=*) TAG="${1#--tag=}"; shift ;;
        --sha256-file) SHA_FILE="${2:-}"; shift 2 ;;
        --sha256-file=*) SHA_FILE="${1#--sha256-file=}"; shift ;;
        --out) OUT="${2:-}"; shift 2 ;;
        --out=*) OUT="${1#--out=}"; shift ;;
        --check) CHECK=1; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) die "unknown argument '$1'" ;;
    esac
done

[ -n "$TAG" ] || die "--tag is required (e.g. --tag v0.14.0)"
# Strict, full-string vX.Y.Z (digits only, exactly three dotted numeric fields).
# grep -qE with ^...$ rejects any trailing or injection-shaped characters that a
# permissive shell glob would accept.
printf '%s' "$TAG" | grep -qE '^v[0-9]+\.[0-9]+\.[0-9]+$' \
    || die "invalid --tag '$TAG' (expected strict vX.Y.Z, digits only)"
VERSION=${TAG#v}

# ── Resolve hull.sha256 ──────────────────────────────────────────────────────
manifest_tmp=""
# NB: return 0 so a false test does not leak as the script's exit status when the
# cleanup runs from the EXIT trap (the emit path has no explicit exit).
cleanup() { if [ -n "$manifest_tmp" ]; then rm -f "$manifest_tmp"; fi; return 0; }
trap cleanup EXIT INT TERM

if [ -n "$SHA_FILE" ]; then
    [ -f "$SHA_FILE" ] || die "hull.sha256 file not found: $SHA_FILE"
    MANIFEST="$SHA_FILE"
else
    manifest_tmp=$(mktemp)
    MANIFEST="$manifest_tmp"
    url="https://github.com/$REPO/releases/download/$TAG/hull.sha256"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" >"$MANIFEST" || die "could not download $url"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$MANIFEST" "$url" || die "could not download $url"
    else
        die "need curl or wget to fetch hull.sha256 (or pass --sha256-file)"
    fi
    [ -s "$MANIFEST" ] || die "downloaded hull.sha256 is empty (does $TAG exist?)"
fi

# Count EVERY entry whose asset field is hull-cosmo (valid OR malformed): a
# hull.sha256 line is "<hash>  <name>", so match on the final field regardless of
# the hash. Require exactly one, so a valid entry PLUS a malformed duplicate that
# also names hull-cosmo still fails closed.
named=$(awk '$NF=="hull-cosmo"{c++} END{print c+0}' "$MANIFEST")
[ "$named" -eq 1 ] || die "expected exactly one hull-cosmo entry in hull.sha256, found $named"
# Now validate that single entry's COMPLETE shape: 64-hex hash, two spaces, exact
# name, nothing else. A non-hex or wrong-length hash fails here.
line=$(awk '$NF=="hull-cosmo"{print; exit}' "$MANIFEST")
printf '%s' "$line" | grep -qE '^[0-9a-fA-F]{64}  hull-cosmo$' \
    || die "malformed hull-cosmo entry (expected '<64-hex>  hull-cosmo'): $line"
SHA_LOWER=$(printf '%s' "$line" | awk '{print $1}' | tr 'A-F' 'a-f')
SHA_UPPER=$(printf '%s' "$SHA_LOWER" | tr 'a-f' 'A-F')

# ── Emit ─────────────────────────────────────────────────────────────────────
render() {
    # render <template> <dest>
    sed -e "s/@VERSION@/$VERSION/g" \
        -e "s/@TAG@/$TAG/g" \
        -e "s/@SHA256_LOWER@/$SHA_LOWER/g" \
        -e "s/@SHA256_UPPER@/$SHA_UPPER/g" \
        "$1" >"$2"
}

emit_into() {
    dest="$1"
    mkdir -p "$dest/winget" "$dest/scoop"
    render "$TEMPLATES/Artalis.Hull.yaml.in"            "$dest/winget/Artalis.Hull.yaml"
    render "$TEMPLATES/Artalis.Hull.installer.yaml.in"  "$dest/winget/Artalis.Hull.installer.yaml"
    render "$TEMPLATES/Artalis.Hull.locale.en-US.yaml.in" "$dest/winget/Artalis.Hull.locale.en-US.yaml"
    render "$TEMPLATES/hull.json.in"                    "$dest/scoop/hull.json"
}

if [ "$CHECK" -eq 1 ]; then
    tmp=$(mktemp -d)
    emit_into "$tmp"
    rc=0
    for rel in winget/Artalis.Hull.yaml winget/Artalis.Hull.installer.yaml winget/Artalis.Hull.locale.en-US.yaml scoop/hull.json; do
        if ! diff -u "$SELF_DIR/$rel" "$tmp/$rel" >/dev/null 2>&1; then
            echo "generate.sh: DRIFT: $rel does not match the generated output for $TAG" >&2
            diff -u "$SELF_DIR/$rel" "$tmp/$rel" >&2 || true
            rc=1
        fi
    done
    rm -rf "$tmp"
    [ "$rc" -eq 0 ] && echo "generate.sh: check OK (committed metadata matches $TAG + its hull-cosmo hash)"
    exit "$rc"
fi

emit_into "$OUT"
echo "generate.sh: wrote Winget + Scoop metadata for $TAG (hull-cosmo $SHA_LOWER)"
exit 0
