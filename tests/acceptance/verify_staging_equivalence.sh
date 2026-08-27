#!/bin/sh
# verify_staging_equivalence.sh - READ-ONLY proof that a maintainer-prepopulated
# staging repository's /releases/latest mirrors an official release EXACTLY, so
# the Windows self-update job can point `hull update --repo=<staging>` at it
# (hull update only ever reads a repo's /releases/latest; it cannot select a tag
# or a prerelease).
#
# Fails closed unless ALL hold:
#   - the staging repo's /releases/latest exists and is NOT a prerelease
#   - every asset the reference release has is present in the staging release
#   - each staging asset's SHA-256 EXACTLY matches the reference asset
#   - hull.sha256.sig verifies (source pubkey when passed, else embedded)
#   - the staging binary reports the expected version
#
# Never creates, modifies, or deletes a release. `gh api` GET + HTTP GETs only.
#
# Usage:
#   verify_staging_equivalence.sh <staging_repo> <ref_dir> <expect_version> \
#                                 <workdir> [pubkey_hex]
#   <ref_dir> is a directory already holding the trusted OFFICIAL assets
#   (produced by verify_release_assets.sh) to compare against.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

staging="$1"; ref="$2"; expect_ver="$3"; work="$4"; pubkey="${5:-}"
bins="hull-linux-x86_64 hull-linux-aarch64 hull-darwin-arm64 hull-cosmo"
all="$bins hull.sha256 hull.sha256.sig"

# /releases/latest metadata (read-only)
meta=$(gh api "repos/$staging/releases/latest" 2>/dev/null) \
    || { echo "FAIL: $staging has no /releases/latest (nothing published as latest)"; exit 1; }
pre=$(printf '%s' "$meta" | python3 -c 'import sys,json;print(json.load(sys.stdin)["prerelease"])')
tag=$(printf '%s' "$meta" | python3 -c 'import sys,json;print(json.load(sys.stdin)["tag_name"])')
[ "$pre" = "False" ] \
    || { echo "FAIL: $staging /releases/latest ($tag) is a prerelease; hull update would not select it"; exit 1; }

base="https://github.com/$staging/releases/download/$tag"
mkdir -p "$work"
cd "$work"

# Every reference asset must be present in staging AND byte-identical.
for a in $all; do
    [ -f "$ref/$a" ] || { echo "FAIL: reference set is missing $a"; exit 1; }
    curl -fsSL -o "$a" "$base/$a" \
        || { echo "FAIL: $staging is missing asset '$a'"; exit 1; }
    s=$(sha256sum "$a"       | awk '{print $1}')
    r=$(sha256sum "$ref/$a"  | awk '{print $1}')
    [ "$s" = "$r" ] \
        || { echo "FAIL: $staging $a differs from the official release (staging $s, official $r)"; exit 1; }
done
chmod +x hull-linux-x86_64

if [ -n "$pubkey" ]; then
    ./hull-linux-x86_64 verify-release hull.sha256 hull.sha256.sig --pubkey "$pubkey" \
        || { echo "FAIL: $staging hull.sha256.sig does not verify against the source pubkey"; exit 1; }
else
    ./hull-linux-x86_64 verify-release hull.sha256 hull.sha256.sig \
        || { echo "FAIL: $staging hull.sha256.sig does not verify against the embedded key"; exit 1; }
fi

ver=$(./hull-linux-x86_64 version 2>&1 | head -1)
echo "$ver" | grep -q "$expect_ver" \
    || { echo "FAIL: $staging candidate reports '$ver', expected to contain '$expect_ver'"; exit 1; }

echo "OK: $staging /releases/latest ($tag) mirrors the official release exactly; version '$ver'"
