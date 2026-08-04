#!/bin/sh
# build_cosmocc_bundle.sh <cosmocc-version> <sha256> <out.tar>
#
# Repackages the official Cosmopolitan cosmocc toolchain as a flat ustar bundle
# for `hull tools install cosmocc` -> `hull build` of a cosmo-APE app (the
# counterpart of build_zig_bundle.sh for native targets). cosmocc is the ONLY
# toolchain that can link an APE (obj_emit has no APE format; the compiler-free
# path is native-only), and its binaries are themselves APEs - so ONE arch-free
# bundle serves every host, and a cosmo `hull` can drive it (unlike a native zig
# tree). See docs/features_and_flavors.md + the memory follow-up.
#
#   cosmocc-version  e.g. 4.0.2 (keep in lockstep with mk/fetch.mk COSMOCC_VERSION)
#   sha256           the pinned zip digest (mk/fetch.mk COSMOCC_SHA256)
#   out.tar          output bundle path (published as hull-cosmocc.tar)
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

version="${1:?usage: build_cosmocc_bundle.sh <version> <sha256> <out.tar>}"
sha="${2:?missing cosmocc sha256}"
out="${3:?missing output tar path}"

command -v unzip >/dev/null 2>&1 || { echo "build_cosmocc_bundle: need unzip" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

url="https://cosmo.zip/pub/cosmocc/cosmocc-${version}.zip"
echo "build_cosmocc_bundle: fetching $url"
curl -fsSL --retry 3 --retry-all-errors --retry-delay 2 "$url" -o "$work/cosmocc.zip"

# Verify the pinned SHA-256 (the same trust anchor mk/fetch.mk uses).
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$work/cosmocc.zip" | cut -d' ' -f1)
else
    actual=$(shasum -a 256 "$work/cosmocc.zip" | cut -d' ' -f1)
fi
if [ "$actual" != "$sha" ]; then
    echo "build_cosmocc_bundle: SHA-256 mismatch" >&2
    echo "  expected: $sha" >&2
    echo "  actual:   $actual" >&2
    exit 1
fi
echo "build_cosmocc_bundle: SHA-256 OK"

# cosmocc-<ver>.zip extracts to a tree rooted at bin/ include/ lib/ libexec/ ...
mkdir -p "$work/tree"
unzip -q -o "$work/cosmocc.zip" -d "$work/tree"

# Sanity: the driver + apelink must be present + executable. hull resolves the
# bundle via the sentinel bundle_entry bin/cosmocc.
test -x "$work/tree/bin/cosmocc" || { echo "build_cosmocc_bundle: no bin/cosmocc in tree" >&2; exit 1; }
test -e "$work/tree/bin/apelink"  || echo "build_cosmocc_bundle: warning: bin/apelink missing" >&2

# Re-tar the tree contents (members relative to the root: ./bin/cosmocc, ...) so
# hull's flat/nested extractor lays them down under ~/.hull/tools/cosmocc/.
out_abs=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
tar cf "$out_abs" -C "$work/tree" .
echo "build_cosmocc_bundle: packed $out_abs ($(wc -c < "$out_abs") bytes)"
