#!/bin/sh
# verify_release_assets.sh - READ-ONLY verification of a published release.
#
# Downloads a release's assets and verifies, WITHOUT mutating any release:
#   1. all six assets are present
#      (hull-linux-x86_64 / -aarch64 / hull-darwin-arm64 / hull-cosmo /
#       hull.sha256 / hull.sha256.sig)
#   2. hull.sha256.sig verifies (against the source-controlled release pubkey
#      when one is passed, else the candidate binary's embedded key)
#   3. each binary's SHA-256 matches its hull.sha256 line
#   4. the linux-x86_64 binary reports the expected version string
#
# Never creates, modifies, or deletes a release. Only HTTP GETs.
#
# Usage: verify_release_assets.sh <repo> <tag> <expect_version> <workdir> [pubkey_hex]
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

repo="$1"; tag="$2"; expect_ver="$3"; work="$4"; pubkey="${5:-}"
base="https://github.com/$repo/releases/download/$tag"
bins="hull-linux-x86_64 hull-linux-aarch64 hull-darwin-arm64 hull-cosmo"
all="$bins hull.sha256 hull.sha256.sig"

mkdir -p "$work"
cd "$work"

# 1. presence
for a in $all; do
    curl -fsSL -o "$a" "$base/$a" \
        || { echo "FAIL: missing asset '$a' in $repo@$tag"; exit 1; }
done
chmod +x hull-linux-x86_64

# 2. signature - source pubkey if given (verification independent of the
#    artifact's own embedded key), otherwise the embedded key.
if [ -n "$pubkey" ]; then
    ./hull-linux-x86_64 verify-release hull.sha256 hull.sha256.sig --pubkey "$pubkey" \
        || { echo "FAIL: hull.sha256.sig does not verify against the source release pubkey"; exit 1; }
else
    ./hull-linux-x86_64 verify-release hull.sha256 hull.sha256.sig \
        || { echo "FAIL: hull.sha256.sig does not verify against the embedded key"; exit 1; }
fi

# 3. per-binary SHA-256 vs the manifest
for a in $bins; do
    want=$(grep "  $a\$" hull.sha256 | awk '{print $1}')
    [ -n "$want" ] || { echo "FAIL: $a has no line in hull.sha256"; exit 1; }
    got=$(sha256sum "$a" | awk '{print $1}')
    [ "$want" = "$got" ] \
        || { echo "FAIL: $a SHA-256 mismatch (manifest $want, file $got)"; exit 1; }
done

# 4. candidate version
ver=$(./hull-linux-x86_64 version 2>&1 | head -1)
echo "$ver" | grep -q "$expect_ver" \
    || { echo "FAIL: candidate reports '$ver', expected to contain '$expect_ver'"; exit 1; }

echo "OK: $repo@$tag - 6 assets present, signature valid, hashes match, version '$ver'"
