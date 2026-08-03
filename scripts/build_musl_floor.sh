#!/bin/sh
# build_musl_floor.sh <dest-dir> [<tar-out>]
#
# Assemble a self-contained musl static-link floor into <dest-dir>: the crt
# startup objects + static libc + the compiler runtime, so `hull build
# --linker=lld-static` (Tier B) can link a fully static musl binary with NOTHING
# else installed (no musl-dev, no gcc, no cc). This is what a
# `hull tools install libc-musl-<arch>` bundle contains; it is used by
# tests/e2e_musl.sh and by the release producer.
#
# With a 2nd arg, also pack the floor as a flat ustar archive at <tar-out> -
# the exact `hull-libc-musl-<arch>.tar` asset the installer fetches + extracts.
#
# Must run on a musl system (Alpine) with musl-dev + gcc present. Sources:
#   crt1.o / crti.o / crtn.o / libc.a  <- $HULL_LIBC_DIR (default /usr/lib)
#   libgcc.a                           <- system gcc runtime (glob; cc fallback)
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

DEST="${1:?usage: build_musl_floor.sh <dest-dir>}"
LIBC_DIR="${HULL_LIBC_DIR:-/usr/lib}"

mkdir -p "$DEST"

for f in crt1.o crti.o crtn.o libc.a; do
    if [ ! -r "$LIBC_DIR/$f" ]; then
        echo "build_musl_floor: missing $LIBC_DIR/$f (install musl-dev, or set HULL_LIBC_DIR)" >&2
        exit 1
    fi
    cp "$LIBC_DIR/$f" "$DEST/$f"
done

# musl folds libm/libpthread/rt/dl/... into libc.a and ships EMPTY stub archives
# purely for `-l` compatibility. Bundle whichever exist so ld.lld resolves the
# `-lm -lpthread` (etc.) that build.lua emits without reaching the system
# /usr/lib - the whole point of a self-contained bundle.
for f in libm.a libpthread.a librt.a libdl.a libutil.a libresolv.a libcrypt.a libxnet.a; do
    [ -r "$LIBC_DIR/$f" ] && cp "$LIBC_DIR/$f" "$DEST/$f" || true
done

# libgcc.a: glob the system gcc runtime dir first (no compiler spawn); fall back
# to `cc -print-libgcc-file-name`. Mirrors linker_lld.c's discovery order.
LIBGCC="$(ls /usr/lib/gcc/*/*/libgcc.a 2>/dev/null | head -1 || true)"
if [ -z "$LIBGCC" ]; then
    LIBGCC="$(cc -print-libgcc-file-name 2>/dev/null || true)"
fi
if [ -z "$LIBGCC" ] || [ ! -r "$LIBGCC" ]; then
    echo "build_musl_floor: no libgcc.a found (install gcc)" >&2
    exit 1
fi
cp "$LIBGCC" "$DEST/libgcc.a"

echo "build_musl_floor: assembled a self-contained floor in $DEST:"
ls -1 "$DEST"

# Optional: pack a flat ustar archive (the release asset). `-C "$DEST" .` makes
# every member a bare "./<file>" - no absolute paths, no leading dirs - which is
# exactly what hl_tools_extract_tar expects (it strips the "./" and refuses any
# other path component).
TAR_OUT="${2:-}"
if [ -n "$TAR_OUT" ]; then
    tar cf "$TAR_OUT" -C "$DEST" .
    echo "build_musl_floor: packed $TAR_OUT ($(wc -c < "$TAR_OUT") bytes)"
fi
