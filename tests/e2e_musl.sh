#!/bin/sh
# E2E test: Hull builds and runs on musl (Alpine).
#
# Hull is developed on glibc + macOS; musl's stricter dynamic loader exposes
# bugs those never hit (e.g. the .rodata-vs-.data.rel.ro relocation crash fixed
# in the compiler-free emitter). This test locks musl support in CI.
#
# It builds hull under musl, then builds two apps through the DEFAULT (emit)
# path and runs them:
#   1. a compute app.main   - proves emit-path binaries load on musl (the
#      relocation-in-.data.rel.ro regression: was SIGSEGV in ld-musl do_relocs).
#   2. an HTTP server app    - proves the full server runtime serves a request.
#
# Dual mode: on a musl host it builds directly; elsewhere (dev macOS/glibc, CI
# ubuntu) it re-execs itself inside an alpine:3.20 Docker container. So the CI
# job is a plain ubuntu runner (avoids actions/checkout's glibc-node breakage on
# an alpine container), and `make e2e-musl` just works on a dev box with Docker.
#
# Usage: sh tests/e2e_musl.sh   /   make e2e-musl
# See docs/musl_build.md.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"

# On a musl Linux? Definitive: macOS is Darwin and glibc Linux has no
# /lib/ld-musl-*, so both correctly resolve to 0. HULL_MUSL_INNER is a recursion
# sentinel: the Docker re-exec sets it so the inner run never loops even if this
# probe were ever wrong.
ON_MUSL=0
if [ "$(uname -s)" = "Linux" ] && ls /lib/ld-musl-* >/dev/null 2>&1; then
    ON_MUSL=1
fi

# ── Not on musl: re-exec inside Alpine via Docker ──────────────────────────
if [ "${HULL_MUSL_INNER:-0}" != "1" ] && [ "$ON_MUSL" != "1" ]; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "SKIP: host is not musl and docker is unavailable"
        exit 0
    fi
    echo "== e2e_musl: host is not musl - re-exec inside alpine:3.20 =="
    exec docker run --rm -e HULL_MUSL_INNER=1 -v "$SRCDIR":/src:ro alpine:3.20 sh -c '
        set -u
        apk add --no-cache build-base clang lld make xxd bash git perl linux-headers \
            >/dev/null 2>&1 || { echo "SKIP: apk add failed (network?)"; exit 0; }
        # Copy the read-only mount to a writable layer and strip any host-built
        # (macOS/glibc) objects so everything recompiles native under musl.
        cp -a /src/. /work/ 2>/dev/null || true
        cd /work
        find . -name "*.o" -delete 2>/dev/null || true
        find . -name "*.a" -delete 2>/dev/null || true
        rm -rf build
        exec sh tests/e2e_musl.sh
    '
fi

# ── On musl from here ──────────────────────────────────────────────────────
PASS=0; FAIL=0; WORKDIR=""; SRV=""
cleanup() {
    [ -n "$SRV" ] && { kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; }
    [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ] && rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM
ok()  { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }

JOBS="$(nproc 2>/dev/null || echo 2)"
echo "== building hull on musl ($(cc -dumpmachine 2>/dev/null)) =="
if ! make -j"$JOBS" >/tmp/musl_mk.log 2>&1; then
    echo "FAIL: make (musl build)"; tail -25 /tmp/musl_mk.log; exit 1
fi
if ! make platform -j"$JOBS" >/tmp/musl_mp.log 2>&1; then
    echo "FAIL: make platform (musl)"; tail -25 /tmp/musl_mp.log; exit 1
fi
HULL="$SRCDIR/build/hull"
"$HULL" version || { echo "FAIL: hull does not run on musl"; exit 1; }
echo

WORKDIR="$(mktemp -d)"

# [1] compute app.main via the emit path - the #192 regression lock.
mkdir -p "$WORKDIR/compute"
printf 'app.manifest({ modules = {} })\napp.main(function(ctx) ctx.stdout:write("musl compute ok\\n"); return 0 end)\n' \
    > "$WORKDIR/compute/app.lua"
if "$HULL" build "$WORKDIR/compute" --no-verify-platform -o "$WORKDIR/compute/bin" \
        >/tmp/musl_c.log 2>&1; then
    out="$("$WORKDIR/compute/bin" --no-sandbox 2>/dev/null || true)"
    if [ "$out" = "musl compute ok" ]; then
        ok "compute emit-path app builds + runs on musl (no ld-musl reloc crash)"
    else
        bad "compute emit-path app ran but output was '$out'"
    fi
else
    bad "compute emit-path app build"; tail -10 /tmp/musl_c.log
fi

# [2] HTTP server via the emit path - full server runtime on musl.
mkdir -p "$WORKDIR/http"
printf 'app.manifest({ modules = { "hull/http-server@1" } })\napp.get("/", function(req, res) res:text("hi from musl") end)\n' \
    > "$WORKDIR/http/app.lua"
if "$HULL" build "$WORKDIR/http" --no-verify-platform -o "$WORKDIR/http/srv" \
        >/tmp/musl_h.log 2>&1; then
    "$WORKDIR/http/srv" -p 8137 --no-sandbox >/tmp/musl_srv.log 2>&1 &
    SRV=$!
    sleep 1
    body="$(wget -qO- http://127.0.0.1:8137/ 2>/dev/null || true)"
    kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; SRV=""
    if [ "$body" = "hi from musl" ]; then
        ok "HTTP server emit-path app serves a request on musl"
    else
        bad "HTTP server responded '$body' (expected 'hi from musl')"; tail -6 /tmp/musl_srv.log
    fi
else
    bad "HTTP server app build"; tail -10 /tmp/musl_h.log
fi

echo
echo "== e2e_musl: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
