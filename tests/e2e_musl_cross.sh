#!/bin/sh
# e2e_musl_cross.sh - the musl cross-build loop (audit #4c).
#
# End-to-end proof that a glibc host can `hull build --target=<arch>-linux-musl
# --linker=zig` and produce a STATIC musl ELF that runs on Alpine, using the
# musl platform archive set (#4a producer + #4c fetch/select). Steps:
#   1. build the musl archive set with scripts/build_musl_platform.sh (Docker
#      Alpine) and stage it at ~/.hull/tools/platform-musl-x86_64/ (where
#      `hull tools install platform-musl-x86_64` would place it),
#   2. stage a system zig into ~/.hull/tools/zig/ (the turnkey cross-linker),
#   3. cross-build a pure compute app.main for x86_64-linux-musl,
#   4. RUN the result inside alpine:3.20 (musl) and assert its output.
#
# Runs on a Linux x86_64 host with Docker (needs Docker to build the musl set +
# run the musl result). SKIPS cleanly elsewhere / when zig or Docker is absent.
# The full musl archive-set build is ~minutes; cached across runs (skips the
# rebuild when the staged set already exists).
#
# Requires: an EMBED_PLATFORM=1 hull, Docker, a system zig (0.13.x).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$SRCDIR/build/hull}"
PASS=0; FAIL=0
assert() { msg="$1"; shift; if "$@"; then echo "  ok  $msg"; PASS=$((PASS+1)); \
    else echo "  FAIL $msg"; FAIL=$((FAIL+1)); fi; }

[ -x "$HULL" ] || { echo "FAIL: $HULL not found - run 'make' first"; exit 1; }

HOST_OS=$(uname -s); HOST_ARCH=$(uname -m)
if [ "$HOST_OS" != "Linux" ] || { [ "$HOST_ARCH" != "x86_64" ] && [ "$HOST_ARCH" != "amd64" ]; }; then
    echo "SKIP: musl cross e2e runs on a Linux x86_64 host (needs Docker Alpine +"
    echo "      the musl archive set). Host: $HOST_OS/$HOST_ARCH"
    exit 0
fi
command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit 0; }
docker info >/dev/null 2>&1 || { echo "SKIP: docker daemon not running"; exit 0; }

find_zig() {
    p=$(command -v zig 2>/dev/null) && { echo "$p"; return 0; }
    for d in /opt/zig /opt/homebrew/bin /usr/local/bin; do
        [ -x "$d/zig" ] && { echo "$d/zig"; return 0; }
    done
    return 1
}
ZIG_BIN=$(find_zig) || { echo "SKIP: no system zig found (install from ziglang.org)"; exit 0; }
echo "── using zig: $ZIG_BIN ($("$ZIG_BIN" version 2>/dev/null)) ──"

ARCH=x86_64
PM_DIR="$HOME/.hull/tools/platform-musl-$ARCH"
ZIG_DIR="$HOME/.hull/tools/zig"
STAGED_ZIG=""; STAGED_PM=""
cleanup() {
    [ -n "$STAGED_ZIG" ] && rm -f "$STAGED_ZIG" 2>/dev/null || true
    [ -n "$STAGED_PM" ] && rm -rf "$PM_DIR" 2>/dev/null || true
    rm -rf "${WORKDIR:-}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# 1. Build + stage the musl archive set (cached: skip if already present).
if [ -f "$PM_DIR/libhull_platform.a" ]; then
    echo "── musl archive set already staged at $PM_DIR ──"
else
    echo "── building the musl archive set (scripts/build_musl_platform.sh, Alpine) ──"
    OUT_TAR="$(mktemp -d)/hull-platform-musl-$ARCH.tar"
    docker run --rm -v "$SRCDIR":/work:ro -v "$(dirname "$OUT_TAR")":/out alpine:3.20 sh -c '
        set -e
        apk add --no-cache build-base clang lld make xxd bash git perl linux-headers >/dev/null 2>&1
        sh /work/scripts/build_musl_platform.sh /work /out/'"$(basename "$OUT_TAR")"'
    '
    mkdir -p "$PM_DIR"; tar xf "$OUT_TAR" -C "$PM_DIR"; STAGED_PM="$PM_DIR"
    rm -rf "$(dirname "$OUT_TAR")"
fi
assert "musl base staged"          [ -f "$PM_DIR/libhull_platform.a" ]
assert "musl lua runtime staged"   [ -f "$PM_DIR/libhull_feature-lua.a" ]

# 2. Stage zig (symlink; zig finds its lib/ via the real exe path).
mkdir -p "$ZIG_DIR"
if [ ! -e "$ZIG_DIR/zig" ]; then ln -sf "$ZIG_BIN" "$ZIG_DIR/zig"; STAGED_ZIG="$ZIG_DIR/zig"; fi

# 3. Cross-build a pure compute app.main for x86_64-linux-musl.
WORKDIR="$(mktemp -d)"
APP="$WORKDIR/app"; mkdir -p "$APP"
cat > "$APP/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() print("musl-cross-ok"); return 0 end)
LUA

# Skip cleanly if this hull can't compose (no embedded platform lib).
probe="$("$HULL" build "$APP" --target=x86_64-linux-musl --linker=zig -o "$WORKDIR/probe" 2>&1 || true)"
case "$probe" in
    *"platform library not embedded"*|*"cannot find libhull_platform.a"*)
        echo "SKIP: hull not built with EMBED_PLATFORM=1"; exit 0 ;;
esac

echo "== hull build --target=x86_64-linux-musl --linker=zig =="
BIN="$WORKDIR/app-musl"
OUT="$("$HULL" build "$APP" --target=x86_64-linux-musl --linker=zig -o "$BIN" 2>&1 || true)"
echo "$OUT" | sed 's/^/    /'
assert "build wrote the binary"        [ -f "$BIN" ]
if [ -f "$BIN" ]; then
    # Must be a static x86_64 ELF (cross-produced from the glibc host).
    assert "is an ELF x86-64 binary"   sh -c "file '$BIN' | grep -q 'ELF 64-bit.*x86-64'"
    assert "is statically linked"      sh -c "file '$BIN' | grep -q 'statically linked'"

    # 4. RUN it inside Alpine (musl). --no-sandbox: an unprivileged container
    #    has no landlock, so pledge/unveil sealing fails (a container limit, not
    #    a musl issue - same convention as e2e_musl.sh).
    echo "== run the musl binary in alpine:3.20 =="
    RUN="$(docker run --rm -v "$WORKDIR":/app:ro alpine:3.20 /app/app-musl --no-sandbox 2>&1 || true)"
    echo "$RUN" | sed 's/^/    /'
    assert "app.main ran on musl"      sh -c "printf '%s' '$RUN' | grep -q 'musl-cross-ok'"
fi

echo ""
echo "e2e_musl_cross: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
