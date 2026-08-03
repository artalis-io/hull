#!/bin/sh
# build_lld_bundle.sh <llvm-triple> <llvm-version> <out.tar>
#
# Packs the LLVM lld linker binaries as a flat ustar bundle for
# `hull tools install lld` -> `hull build --linker=lld`. The bundle holds the
# dotless `lld` driver plus its personalities (`ld.lld` ELF, `ld64.lld` Mach-O,
# `wasm-ld`) at the tree root; hull's linker backend resolves
# $HOME/.hull/tools/lld/lld and uses that dir as the -B prefix so the
# personalities sit alongside.
#
#   llvm-triple   LLVM release arch tag (see the RELEASE matrix in release.yml),
#                 e.g. x86_64-linux-gnu-ubuntu-18.04 | aarch64-linux-gnu |
#                 arm64-apple-macos11
#   llvm-version  e.g. 18.1.8
#   out.tar       output bundle path (published as hull-lld-<hull-platform>.tar)
#
# Local validation: set LLD_SRC_DIR=<dir> to pack lld binaries already present
# in <dir> (e.g. a Homebrew/apt lld's bin dir) instead of downloading LLVM -
# exercises the deref + flat-pack + layout without the ~1 GB fetch.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

triple="${1:?usage: build_lld_bundle.sh <llvm-triple> <llvm-version> <out.tar>}"
version="${2:?missing llvm version}"
out="${3:?missing output tar path}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
src="$work/src"          # where the lld binaries land before deref
mkdir -p "$src"

if [ -n "${LLD_SRC_DIR:-}" ]; then
    # Local test mode: copy from an existing bin dir.
    echo "build_lld_bundle: packing from LLD_SRC_DIR=$LLD_SRC_DIR"
    # -L derefs so a symlinked lld (Homebrew/apt) lands as a real file.
    for f in lld ld.lld ld64.lld wasm-ld; do
        [ -e "$LLD_SRC_DIR/$f" ] && cp -L "$LLD_SRC_DIR/$f" "$src/$f" || true
    done
else
    url="https://github.com/llvm/llvm-project/releases/download/llvmorg-${version}/clang+llvm-${version}-${triple}.tar.xz"
    echo "build_lld_bundle: fetching $url"
    curl -fsSL --retry 3 --retry-all-errors --retry-delay 2 "$url" -o "$work/llvm.tar.xz"
    # Extract only the lld binaries. GNU tar needs --wildcards for globs; BSD
    # tar (macOS) globs by default and rejects the flag - detect and branch.
    if tar --version 2>/dev/null | grep -qi 'gnu'; then
        tar -xJf "$work/llvm.tar.xz" -C "$src" --strip-components=2 --wildcards \
            '*/bin/lld' '*/bin/ld.lld' '*/bin/ld64.lld' '*/bin/wasm-ld' 2>/dev/null || true
    else
        tar -xJf "$work/llvm.tar.xz" -C "$src" --strip-components=2 \
            '*/bin/lld' '*/bin/ld.lld' '*/bin/ld64.lld' '*/bin/wasm-ld' 2>/dev/null || true
    fi
fi

# The personalities (ld.lld / ld64.lld / wasm-ld) ship as symlinks to `lld`;
# deref to standalone copies so the flat bundle has no dangling links.
flat="$work/flat"
mkdir -p "$flat"
for f in lld ld.lld ld64.lld wasm-ld; do
    if [ -e "$src/$f" ]; then
        cp -L "$src/$f" "$flat/$f"
        chmod +x "$flat/$f"
    fi
done
test -x "$flat/lld" || { echo "build_lld_bundle: no lld driver extracted" >&2; exit 1; }

out_abs=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
( cd "$flat" && tar -cf "$out_abs" . )
echo "build_lld_bundle: wrote $out_abs ($(du -h "$out_abs" | cut -f1))"
