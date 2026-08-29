#!/bin/sh
# populate_staging.sh - MAINTAINER helper to mirror an official release's exact
# signed assets into a staging repository as THAT repo's latest non-prerelease,
# so the release-acceptance run can point `hull update --repo=<staging>` at it
# (hull update only reads /releases/latest; it cannot select a tag/prerelease).
#
# This is the ONE mutating step of the acceptance run and it is deliberately
# NOT done by CI: it runs interactively, with the maintainer's own `gh` auth, so
# there is no persistent cross-repository token. The acceptance workflows
# themselves stay read-only. Cleanup (deleting the staging release after final
# promotion) is likewise a manual maintainer step.
#
# It verifies the official assets before mirroring (signature + per-binary
# SHA-256 + version, via verify_release_assets.sh) so only authentic bytes are
# ever copied, and it REFUSES to clobber an existing staging release of the same
# tag (delete it first if you are re-mirroring a fresh RC).
#
# Usage:
#   tests/acceptance/populate_staging.sh <official_repo> <tag> <staging_repo> <expect_version>
# Example:
#   tests/acceptance/populate_staging.sh artalis-io/hull v0.14.0-rc1 \
#       artalis-io/hull-release-rc-staging 0.14.0-rc1
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

if [ $# -ne 4 ]; then
    echo "usage: $0 <official_repo> <tag> <staging_repo> <expect_version>" >&2
    exit 2
fi
official="$1"; tag="$2"; staging="$3"; expect_ver="$4"
here=$(cd "$(dirname "$0")" && pwd)
bins="hull-linux-x86_64 hull-linux-aarch64 hull-darwin-arm64 hull-cosmo"
all="$bins hull.sha256 hull.sha256.sig"

command -v gh >/dev/null || { echo "FAIL: gh CLI not found" >&2; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "FAIL: gh is not authenticated" >&2; exit 1; }

# Source-controlled release pubkey (from a repo checkout), else the embedded key.
pubkey=""
if [ -f "$here/../../include/hull/release.h" ]; then
    pubkey=$(python3 - "$here/../../include/hull/release.h" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
m = re.search(r'define\s+HL_RELEASE_PUBKEY_HEX(.*?)(?=\n#|\n\n|\Z)', s, re.S)
h = ''.join(re.findall(r'"([0-9a-fA-F]*)"', m.group(1))) if m else ''
print(h if len(h) == 64 else '')
PY
    )
fi

# 1. Refuse to clobber an existing staging release of this tag (fail closed).
if gh release view "$tag" --repo "$staging" >/dev/null 2>&1; then
    echo "FAIL: $staging already has a release tagged $tag." >&2
    echo "      Delete it first if re-mirroring: gh release delete $tag --repo $staging --cleanup-tag --yes" >&2
    exit 1
fi

# 2. Download + verify the official assets (signature, hashes, version).
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
echo "populate_staging: verifying official $official@$tag before mirroring ..."
sh "$here/verify_release_assets.sh" "$official" "$tag" "$expect_ver" "$work" "$pubkey"

# 3. Create the staging release as latest NON-prerelease with the exact bytes.
files=""
for a in $all; do files="$files $work/$a"; done
echo "populate_staging: creating $staging release $tag (latest, non-prerelease) ..."
# shellcheck disable=SC2086
gh release create "$tag" --repo "$staging" \
    --title "$tag (staging mirror; not for distribution)" \
    --notes "Exact signed assets mirrored from $official@$tag for release acceptance. Do not distribute." \
    $files

# 4. Confirm the staging repo now serves it as a non-prerelease latest.
meta=$(gh api "repos/$staging/releases/latest" 2>/dev/null) \
    || { echo "FAIL: $staging still has no /releases/latest after create" >&2; exit 1; }
pre=$(printf '%s' "$meta" | python3 -c 'import sys,json;print(json.load(sys.stdin)["prerelease"])')
lat=$(printf '%s' "$meta" | python3 -c 'import sys,json;print(json.load(sys.stdin)["tag_name"])')
if [ "$pre" != "False" ]; then echo "FAIL: $staging latest is a prerelease" >&2; exit 1; fi
if [ "$lat" != "$tag" ]; then echo "FAIL: $staging /releases/latest is '$lat', expected '$tag'" >&2; exit 1; fi

echo "OK: mirrored $official@$tag -> $staging (/releases/latest = $lat, non-prerelease)"
