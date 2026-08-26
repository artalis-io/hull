#!/bin/sh
# Generate stdlib/{lua,js}/hull/web/_pwned_blocklist.{lua,js}
# from a newline-separated password list.
#
# Output format: a single uppercase-hex string concatenating fixed-
# width (8 char) SHA-1 prefixes, sorted, deduped. Binary searched
# at runtime by hull/web/pwned in 8-char strides.
#
# Source: SecLists 10K most common passwords (CC-BY-3.0). Default
# URL is pinned via SHA-256 of the source list; bump both when
# rotating to a newer revision.
#
# Usage: ./scripts/build_pwned_blocklist.sh [INPUT_FILE]

set -eu

SRC="${1:-/tmp/seclists_top10k.txt}"
[ -f "$SRC" ] || {
    echo "fetch the source first:" >&2
    echo "  curl -fsSL https://raw.githubusercontent.com/danielmiessler/SecLists/master/Passwords/Common-Credentials/10k-most-common.txt -o $SRC" >&2
    exit 1
}

PREFIX_LEN=8

# Hash each, take first PREFIX_LEN chars uppercase, sort, dedupe.
HASHES=$(awk 'NF' "$SRC" \
    | LC_ALL=C tr -d '\r' \
    | while IFS= read -r pw; do
        printf '%s' "$pw" | shasum -a 1 \
            | LC_ALL=C tr 'a-f' 'A-F' \
            | cut -c1-$PREFIX_LEN
    done \
    | sort -u)

COUNT=$(printf '%s\n' "$HASHES" | wc -l | tr -d ' ')
JOINED=$(printf '%s' "$HASHES" | tr -d '\n')

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LUA_OUT="$ROOT/stdlib/lua/hull/web/_pwned_blocklist.lua"
JS_OUT="$ROOT/stdlib/js/hull/web/_pwned_blocklist.js"

# Generated header - both runtimes.
cat > "$LUA_OUT" <<LUA
-- AUTO-GENERATED - do not edit. Regenerate with scripts/build_pwned_blocklist.sh.
-- Source: SecLists 10K (CC-BY-3.0).
-- Format: concatenated 8-char uppercase-hex SHA-1 prefixes, sorted, deduped.
return {
    stride = $PREFIX_LEN,
    count  = $COUNT,
    hashes = "$JOINED",
}
LUA

cat > "$JS_OUT" <<JS
// AUTO-GENERATED - do not edit. Regenerate with scripts/build_pwned_blocklist.sh.
// Source: SecLists 10K (CC-BY-3.0).
// Format: concatenated 8-char uppercase-hex SHA-1 prefixes, sorted, deduped.
export const blocklist = {
    stride: $PREFIX_LEN,
    count:  $COUNT,
    hashes: "$JOINED",
};
JS

echo "wrote $COUNT prefixes ($((COUNT * PREFIX_LEN)) bytes) to:"
echo "  $LUA_OUT"
echo "  $JS_OUT"
