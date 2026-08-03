#!/bin/sh
# build_musl_platform.sh <src-dir> [<tar-out>]
#
# Build the FULL musl platform archive set from a Hull checkout: the composable
# base + the SLIM app-build base + every embedded feature archive, all compiled
# against musl libc. This is the producer for audit item #4 (docs/build_arc_audit.md)
# - a musl `libhull_platform.a` + feature archives so an install-only user can
# `hull build --target=<arch>-linux-musl` (or `--linker=lld-static`) WITHOUT
# building hull from musl source. The companion `build_musl_floor.sh` produces the
# musl libc floor (crt + libc.a); this produces Hull's own archives.
#
# Mirrors build_musl_floor.sh's contract: run inside a musl system (Alpine) with
# the full C toolchain (build-base) present. With a 2nd arg, pack the produced
# archives as a flat ustar at <tar-out> (every member a bare "./<file>", what
# hl_tools_extract_tar expects).
#
# Produced archive set (matches the release producer's Linux native build -
# release.yml `build-platform-native`, the `make platform` / `platform-slim` /
# `feature-embedded` steps):
#   libhull_platform.a              <- make platform      (composable base)
#   libhull_platform-slim.a         <- make platform-slim (SQLite-less+TLS-less base)
#   libhull_feature-*.a             <- make feature-embedded (FEATURE_EMBEDDED_STEMS)
#
# READ-ONLY-SAFE: the build runs in a container-internal copy of <src-dir>, so
# <src-dir> may be mounted read-only. This keeps a host checkout's build/ from
# being clobbered by the in-container musl .o/.a (a macOS host's build/ would
# otherwise break - see the repo's Docker-repro convention). CI mounts $PWD and
# passes it as <src-dir>; the copy costs a few seconds and buys the RO guarantee.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

SRC="${1:?usage: build_musl_platform.sh <src-dir> [<tar-out>]}"
TAR_OUT="${2:-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [ ! -f "$SRC/Makefile" ]; then
    echo "build_musl_platform: '$SRC' is not a Hull checkout (no Makefile)" >&2
    exit 1
fi

# Build in a writable copy so <src-dir> can be a read-only mount.
BUILD_SRC="${HULL_MUSL_BUILD_DIR:-/tmp/hull-musl-build}"
rm -rf "$BUILD_SRC"
mkdir -p "$BUILD_SRC"
# `cp -a .../.` copies the tree contents (incl. dotfiles + submodules) into the
# fresh dir. Vendored submodules (keel, wamr) must already be checked out in SRC.
cp -a "$SRC"/. "$BUILD_SRC"/
cd "$BUILD_SRC"

# A stray build/ carried in from the (possibly host) SRC would mix foreign-libc
# objects with the musl ones. Start from clean so every object is musl-built.
make -s clean >/dev/null 2>&1 || true

echo "build_musl_platform: building the musl archive set (-j$JOBS) ..."
make -j"$JOBS" platform
make -j"$JOBS" platform-slim
make -j"$JOBS" feature-embedded

OUT="${HULL_MUSL_OUT_DIR:-/tmp/hull-musl-out}"
rm -rf "$OUT"
mkdir -p "$OUT"
cp build/libhull_platform.a      "$OUT"/
cp build/libhull_platform-slim.a "$OUT"/
# The embedded feature archives, enumerated by the registry (mk/feature.mk's
# FEATURE_EMBEDDED_STEMS) so this stays in lockstep with the release producer.
# shellcheck disable=SC2046
cp $(make -s print-feature-embedded-libs) "$OUT"/

echo "build_musl_platform: assembled the musl archive set in $OUT:"
ls -1 "$OUT"

if [ -n "$TAR_OUT" ]; then
    tar cf "$TAR_OUT" -C "$OUT" .
    echo "build_musl_platform: packed $TAR_OUT ($(wc -c < "$TAR_OUT") bytes)"
fi
