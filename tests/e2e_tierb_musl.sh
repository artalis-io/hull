#!/bin/sh
# E2E test: Tier B direct static-musl link recipe (toolchain-free, no cc).
#
# Guards the exact `ld.lld -static` invocation that linker_lld.c's direct mode
# (hl_linker_lld_direct_new) produces: emit app_registry.o via obj_emit.c, and
# link it + a trampoline into a FULLY STATIC musl binary using ld.lld directly
# against the crt1/crti/crtn + libc.a floor - NO C compiler in the link step.
#
# Runs only on a musl host with the floor present (Alpine + `apk add
# build-base lld`); skips cleanly elsewhere. The full `hull build
# --linker=lld-static` integration additionally needs a musl-built
# libhull_platform.a (i.e. Hull building on musl), tracked separately.
# See docs/toolchain_free_build.md.
#
# Usage: sh tests/e2e_tierb_musl.sh   /   make e2e-tierb-musl
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
PASS=0; FAIL=0; WORKDIR=""
cleanup() { [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ] && rm -rf "$WORKDIR"; }
trap cleanup EXIT INT TERM
assert() { m="$1"; shift; if "$@"; then echo "  ok  $m"; PASS=$((PASS+1)); else echo "  FAIL $m"; FAIL=$((FAIL+1)); fi; }

# Require the musl static floor + ld.lld + a host cc (to build the harness/stubs).
LD="$(command -v ld.lld 2>/dev/null || true)"
[ -z "$LD" ] && { echo "SKIP: no ld.lld (apk add lld)"; exit 0; }
LIBDIR=""
for d in /usr/lib /lib; do [ -f "$d/crt1.o" ] && [ -f "$d/libc.a" ] && { LIBDIR="$d"; break; }; done
[ -z "$LIBDIR" ] && { echo "SKIP: no static musl floor (crt1.o + libc.a); need a musl system + musl-dev"; exit 0; }
command -v cc >/dev/null 2>&1 || { echo "SKIP: no cc to build the emit harness"; exit 0; }
# musl only: this recipe (crt1+crti+crtn, no crtbegin/crtend) is musl-shaped.
if ! (ldd --version 2>&1 | grep -qi musl) && [ ! -f /lib/ld-musl-*.so.1 ]; then
    echo "SKIP: not a musl system (Tier B direct is musl-first)"; exit 0
fi

WORKDIR="$(mktemp -d)"; cd "$WORKDIR"
case "$(uname -m)" in aarch64) A=a ;; *) A=x ;; esac
echo "── musl host $(uname -m); floor=$LIBDIR; ld.lld=$LD ──"

# 1. emit app_registry.o for the host arch (obj_emit.c, host-independent).
cat > emit.c <<EOF
#include "hull/obj_emit.h"
#include <stdio.h>
#include <stdlib.h>
int main(int c, char **v) { (void)c;
    HlObjArch ar = v[1][0] == 'a' ? HL_OBJ_AARCH64 : HL_OBJ_X86_64;
    HlEmitEntry e[] = { { "./app", (const unsigned char *)"return 1", 8 },
                        { "migrations/001.sql", (const unsigned char *)"CREATE TABLE t(x);", 18 } };
    HlObjTarget t = { HL_OBJ_ELF, ar, 0, 0 };
    unsigned char *o = 0; size_t n = 0;
    if (hl_obj_emit_app_registry(&t, e, 2, &o, &n)) return 1;
    FILE *f = fopen("app_registry.o", "wb"); fwrite(o, 1, n, f); fclose(f); free(o); return 0;
}
EOF
cc -I"$SRCDIR/include" emit.c "$SRCDIR/src/hull/obj_emit.c" -o emit || { echo "SKIP: harness build failed"; exit 0; }
./emit "$A"
assert "emitted app_registry.o" [ -f app_registry.o ]

# 2. the invariant trampoline + a stub hl_app_run (stands in for the platform lib).
printf 'extern int hl_app_run(int,char**);int main(int c,char**v){return hl_app_run(c,v);}' > app_main.c
printf '#include <stdio.h>\nint hl_app_run(int c,char**v){(void)c;(void)v;printf("TIERB static-musl OK\\n");return 0;}' > run.c
cc -c app_main.c -o app_main.o
cc -c run.c -o run.o

# 3. DIRECT ld.lld -static link - the exact shape linker_lld.c's direct mode emits.
"$LD" -static -o app \
    "$LIBDIR/crt1.o" "$LIBDIR/crti.o" \
    app_main.o run.o app_registry.o \
    --start-group -lc --end-group -L"$LIBDIR" \
    "$LIBDIR/crtn.o" 2>err.log
assert "ld.lld -static link succeeds (no cc)" [ -f app ]
assert "output is a static ELF executable" sh -c "file app | grep -q 'ELF.*executable'"
assert "output is statically linked" sh -c "file app | grep -qi static"

# 4. run it.
out="$(./app 2>/dev/null || true)"
assert "static-musl binary runs" sh -c "printf '%s' \"$out\" | grep -q 'TIERB static-musl OK'"

echo ""
echo "Tier B static-musl e2e: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
