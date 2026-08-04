#!/bin/sh
# build_cosmocc_bundle.sh <cosmocc-version> <cosmocc-sha256> \
#                         <busybox-url> <busybox-sha256> <out.tar>
#
# Produces the TRIMMED `hull-cosmocc.tar` bundle for `hull tools install cosmocc`
# -> `hull build` of a cosmo-APE app on any host (incl. Windows). cosmocc is the
# ONLY toolchain that can link an APE (obj_emit has no APE format; the
# compiler-free path is native-only), and its binaries are themselves APEs - so
# ONE arch-free bundle serves every host, and a cosmo `hull` can drive it.
#
# Two things this does beyond the upstream zip:
#   1. TRIM. cosmocc-4.0.2 extracts to ~1.37 GB, over the release_io 512 MB
#      download cap; an APE C build touches only ~309 MB (docs/cosmocc_install.md
#      §C). Drop what a C-only Hull build never uses: the non-C compilers
#      (cc1plus / f951 / lto1 / ...), the C++/unwind runtime libs, .dbg debug
#      sidecars, and docs/examples. A hard size gate FAILS the build (with a du
#      breakdown) if the result still exceeds the margin, so the trim can't
#      silently regress past the cap.
#   2. busybox RIDE-ALONG. Drop a pinned busybox-w64 at bin/busybox.exe so a
#      cosmo hull on Windows can drive cosmocc's #!/bin/sh driver through it
#      (item A; the tool platform enum has no "windows" key for a separate
#      busybox tool). Inert on non-Windows hosts.
#
# Symlink-preserving tar: cosmocc's arch compilers (x86_64-unknown-cosmo-cc, ...)
# are symlinks -> cosmocc; hl_tar_extract (item B) recreates them (or copies).
#
#   cosmocc-version  e.g. 4.0.2 (keep in lockstep with mk/fetch.mk COSMOCC_VERSION)
#   cosmocc-sha256   the pinned zip digest (mk/fetch.mk COSMOCC_SHA256)
#   busybox-url      the pinned busybox-w64 PE (frippery.org/files/busybox/...)
#   busybox-sha256   its digest
#   out.tar          output bundle path (published as hull-cosmocc.tar)
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

version="${1:?usage: build_cosmocc_bundle.sh <version> <sha256> <bb-url> <bb-sha256> <out.tar>}"
sha="${2:?missing cosmocc sha256}"
bb_url="${3:?missing busybox url}"
bb_sha="${4:?missing busybox sha256}"
out="${5:?missing output tar path}"

# Safety margin under the release_io 512 MB download cap. If the trimmed tree
# exceeds this, fail loudly rather than ship an asset that won't install.
MAX_MB=490

command -v unzip >/dev/null 2>&1 || { echo "build_cosmocc_bundle: need unzip" >&2; exit 1; }
if command -v sha256sum >/dev/null 2>&1; then SHA="sha256sum"; else SHA="shasum -a 256"; fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

verify() {  # <file> <expected-sha>
    actual=$($SHA "$1" | cut -d' ' -f1)
    if [ "$actual" != "$2" ]; then
        echo "build_cosmocc_bundle: SHA-256 mismatch for $1" >&2
        echo "  expected: $2" >&2
        echo "  actual:   $actual" >&2
        exit 1
    fi
}

# ── cosmocc ──────────────────────────────────────────────────────────────
url="https://cosmo.zip/pub/cosmocc/cosmocc-${version}.zip"
echo "build_cosmocc_bundle: fetching $url"
curl -fsSL --retry 3 --retry-all-errors --retry-delay 2 "$url" -o "$work/cosmocc.zip"
verify "$work/cosmocc.zip" "$sha"
echo "build_cosmocc_bundle: cosmocc SHA-256 OK"

mkdir -p "$work/tree"
unzip -q -o "$work/cosmocc.zip" -d "$work/tree"
test -x "$work/tree/bin/cosmocc" || { echo "build_cosmocc_bundle: no bin/cosmocc in tree" >&2; exit 1; }

# ── busybox ride-along ───────────────────────────────────────────────────
echo "build_cosmocc_bundle: fetching busybox $bb_url"
curl -fsSL --retry 3 --retry-all-errors --retry-delay 2 -L "$bb_url" -o "$work/busybox.exe"
verify "$work/busybox.exe" "$bb_sha"
echo "build_cosmocc_bundle: busybox SHA-256 OK"
cp "$work/busybox.exe" "$work/tree/bin/busybox.exe"
chmod 0755 "$work/tree/bin/busybox.exe"

# ── trim ─────────────────────────────────────────────────────────────────
before=$(du -sm "$work/tree" | cut -f1)
# Non-C compilers + language backends a C-only Hull build never invokes.
find "$work/tree" -type f \( \
        -name 'cc1plus'    -o -name 'cc1obj'  -o -name 'cc1objplus' -o \
        -name 'f951'       -o -name 'lto1'    -o -name 'd21'        -o \
        -name 'go1'        -o -name 'gnat1'   -o -name 'brig1'      -o \
        -name 'gm2'        -o -name 'rust1' \) -delete 2>/dev/null || true
# C++ / unwind runtime libs (Hull links cosmo libc only).
find "$work/tree" -type f \( \
        -name 'libc++*'    -o -name 'libc++abi*' -o -name 'libstdc++*' -o \
        -name 'libunwind*' \) -delete 2>/dev/null || true
# Debug sidecars + docs/examples/tests (never touched by a build).
find "$work/tree" -name '*.dbg' -delete 2>/dev/null || true
for d in share/doc share/man share/info doc docs examples test tests; do
    rm -rf "$work/tree/$d" 2>/dev/null || true
done

after=$(du -sm "$work/tree" | cut -f1)
echo "build_cosmocc_bundle: trimmed ${before} MB -> ${after} MB"

if [ "$after" -gt "$MAX_MB" ]; then
    echo "build_cosmocc_bundle: trimmed tree ${after} MB exceeds ${MAX_MB} MB margin" >&2
    echo "  the denylist did not shed enough; the biggest remaining paths are:" >&2
    du -h --max-depth=2 "$work/tree" 2>/dev/null | sort -rh | head -25 >&2
    echo "  -> widen the trim denylist in scripts/build_cosmocc_bundle.sh" >&2
    exit 1
fi

# ── pack (symlink-preserving) ────────────────────────────────────────────
out_abs=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
# Members relative to the tree root (./bin/cosmocc, ...); no --dereference, so
# the arch-cc symlinks are preserved for hl_tar_extract (item B) to recreate.
tar cf "$out_abs" -C "$work/tree" .
echo "build_cosmocc_bundle: packed $out_abs ($(du -m "$out_abs" | cut -f1) MB)"
