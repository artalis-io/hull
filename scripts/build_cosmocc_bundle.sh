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
#      §C). Rather than guess a keep-list (the ~1 GB of unneeded third-party/C++
#      archives + spare binutils have unpredictable names, and exec'd tools an
#      open()-only trace would miss), TRACE a representative build (strace
#      trace=file, which includes execve) and keep exactly the files it touches,
#      plus every symlink + every header. A hard size gate FAILS the build (with
#      a du breakdown) if the closure exceeds the margin, so it can't silently
#      regress past the cap. Needs strace + realpath in the build environment.
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
# NOTE: busybox is dropped into the tree AFTER the trim below - the trace closure
# only keeps files a Linux build TOUCHES, and busybox (a Windows-only shell) is
# not among them, so copying it in first would get it swept by the trim.

# ── trim (trace-closure) ──────────────────────────────────────────────────
# An APE build opens only ~309 MB of the ~1.37 GB tree (§C); the rest is
# third-party + C++ static archives (~730 MB in the arch lib/ dirs), spare
# binutils (~127 MB in bin/), and non-C compilers - with unpredictable names a
# keep-list would have to guess at (and exec'd tools that an open()-only trace
# would miss). So rather than guess, TRACE a representative build and keep
# exactly the files it touches (open + exec + stat), PLUS every symlink (the
# arch-cc -> cosmocc stubs) and every header (include/ trees are small; keeping
# them whole means no app's headers can go missing). Guaranteed correct + fits
# the 512 MB cap without a keep-list or a cap bump.
command -v strace  >/dev/null 2>&1 || { echo "build_cosmocc_bundle: need strace" >&2; exit 1; }
command -v realpath >/dev/null 2>&1 || { echo "build_cosmocc_bundle: need realpath" >&2; exit 1; }
before=$(du -sm "$work/tree" | cut -f1)

probe="$work/probe"
mkdir -p "$probe"
cat > "$probe/app.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
static void *thr(void *a){ return a; }
int main(void){
  pthread_t t; pthread_create(&t,0,thr,0); pthread_join(t,0);
  char *p = malloc(64); snprintf(p,64,"%f",sqrt(2.0)); size_t n = strlen(p);
  free(p); return (int)n;
}
EOF
# -ff = one log per pid (cosmocc forks the arch drivers / compile / apelink), so
# per-process calls stay sequential. trace=file covers every path-taking syscall
# INCLUDING execve, so the exec'd toolchain binaries (gcc drivers, cc1, as, ld,
# collect2, apelink, fixupobj, pecheck) land in the closure - an open()-only
# trace would drop them and the delete pass would then break the toolchain.
strace -ff -y -e trace=file \
    -o "$probe/tr" "$work/tree/bin/cosmocc" -O2 -o "$probe/probe.com" "$probe/app.c" \
    >/dev/null 2>&1 || { echo "build_cosmocc_bundle: probe build under strace failed" >&2; exit 1; }
test -f "$probe/probe.com" || { echo "build_cosmocc_bundle: probe produced no APE" >&2; exit 1; }

# Keep set = canonicalized paths of successful syscalls under the tree (strace
# paths carry bin/../ forms; realpath canonicalizes to match find's output)
# ∪ every header. Symlinks are never deleted (the delete pass is -type f only).
keep="$work/keep.txt"
cat "$probe"/tr.* \
  | grep -E '\)[[:space:]]*=[[:space:]]*[0-9]' \
  | grep -oE '"[^"]*"' | sed 's/^"//; s/"$//' \
  | grep -F "$work/tree/" \
  | while IFS= read -r p; do realpath "$p" 2>/dev/null || true; done \
  | sort -u > "$keep"
find "$work/tree" -type f -path '*/include/*' >> "$keep"
sort -u "$keep" -o "$keep"

# Delete every regular file NOT in the keep set (symlinks + dirs untouched),
# then prune the directories left empty.
find "$work/tree" -type f | sort -u > "$work/all.txt"
comm -23 "$work/all.txt" "$keep" | tr '\n' '\0' | xargs -0 rm -f 2>/dev/null || true
find "$work/tree" -depth -type d -empty -delete 2>/dev/null || true
rm -rf "$probe"

after=$(du -sm "$work/tree" | cut -f1)
echo "build_cosmocc_bundle: trimmed ${before} MB -> ${after} MB"

if [ "$after" -gt "$MAX_MB" ]; then
    echo "build_cosmocc_bundle: trimmed tree ${after} MB exceeds ${MAX_MB} MB margin" >&2
    echo "  the trace closure is larger than expected; biggest remaining paths:" >&2
    du -h --max-depth=2 "$work/tree" 2>/dev/null | sort -rh | head -25 >&2
    echo "  -> investigate the closure in scripts/build_cosmocc_bundle.sh" >&2
    exit 1
fi

# ── busybox ride-along (AFTER the trim, so it survives) ──────────────────
cp "$work/busybox.exe" "$work/tree/bin/busybox.exe"
chmod 0755 "$work/tree/bin/busybox.exe"
test -x "$work/tree/bin/busybox.exe" || { echo "build_cosmocc_bundle: busybox drop-in failed" >&2; exit 1; }
echo "build_cosmocc_bundle: busybox placed at bin/busybox.exe"

# ── pack (symlink-preserving) ────────────────────────────────────────────
out_abs=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
# Members relative to the tree root (./bin/cosmocc, ...); no --dereference, so
# the arch-cc symlinks are preserved for hl_tar_extract (item B) to recreate.
tar cf "$out_abs" -C "$work/tree" .
echo "build_cosmocc_bundle: packed $out_abs ($(du -m "$out_abs" | cut -f1) MB)"
