#!/bin/sh
# build_zig_bundle.sh <zig-triple> <zig-version> <out.tar>
#
# Repackages the official static Zig toolchain as a flat ustar bundle for
# `hull tools install zig` -> `hull build --linker=zig`. Zig's own release is a
# self-contained, symlink-free tree (the `zig` driver + its `lib/`), which is
# exactly what a Hull tool bundle wants; we just download it and re-tar the tree
# root so hull's extractor lays it down at $HOME/.hull/tools/zig/.
#
#   zig-triple   ziglang.org arch tag: linux-x86_64 | linux-aarch64 | macos-aarch64
#   zig-version  e.g. 0.13.0 (keep in lockstep with the version CI installs)
#   out.tar      output bundle path (published as hull-zig-<hull-platform>.tar)
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

triple="${1:?usage: build_zig_bundle.sh <zig-triple> <zig-version> <out.tar>}"
version="${2:?missing zig version}"
out="${3:?missing output tar path}"

tarball="zig-${triple}-${version}.tar.xz"

# The tarball must match its pin in scripts/zig.sha256 (the repacked tree is
# signed into hull.sha256, so an unpinned download would be release-signed as
# whatever the server sent). No pin for this triple/version: refuse.
pins="$(cd "$(dirname "$0")" && pwd)/zig.sha256"
expected=$(awk -v f="$tarball" '$1 !~ /^#/ && $2 == f { print $1 }' "$pins")
case "$expected" in *[!0-9a-f]*) expected="" ;; esac
[ ${#expected} -eq 64 ] || expected=""
[ -n "$expected" ] || { echo "build_zig_bundle: no SHA-256 pin for $tarball in $pins" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

url="https://ziglang.org/download/${version}/${tarball}"
echo "build_zig_bundle: fetching $url"
# --retry-all-errors also retries a partial transfer (curl exit 18), the
# observed ziglang.org download flake.
curl -fsSL --retry 3 --retry-all-errors --retry-delay 2 "$url" -o "$work/zig.tar.xz"

if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$work/zig.tar.xz" | awk '{print $1}')
else
    actual=$(shasum -a 256 "$work/zig.tar.xz" | awk '{print $1}')
fi
if [ "$actual" != "$expected" ]; then
    echo "build_zig_bundle: SHA-256 mismatch for $tarball" >&2
    echo "  expected: $expected" >&2
    echo "  actual:   $actual" >&2
    exit 1
fi
echo "build_zig_bundle: SHA-256 verified ($expected)"

mkdir -p "$work/tree"
# --strip-components=1 drops the top-level zig-<triple>-<ver>/ dir so the tree
# root holds `zig` + `lib/` directly.
tar -xJf "$work/zig.tar.xz" -C "$work/tree" --strip-components=1

# Sanity: the driver must be present and executable.
test -x "$work/tree/zig" || { echo "build_zig_bundle: no zig driver in tree" >&2; exit 1; }

# Re-tar the tree contents (members relative to the root: ./zig, ./lib/...) so
# hull's flat/nested extractor writes them under ~/.hull/tools/zig/.
out_abs=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
( cd "$work/tree" && tar -cf "$out_abs" . )
echo "build_zig_bundle: wrote $out_abs ($(du -h "$out_abs" | cut -f1))"
