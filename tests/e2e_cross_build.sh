#!/bin/sh
# E2E test: cross-compilation foundation - emit app_registry.o for a FOREIGN
# (arch) than the host and prove a cross-linker (lld) accepts it.
#
# obj_emit.c is host-independent: it emits ELF for x86_64 or aarch64 from any
# host. This test emits both arches, cross-links each with `ld.lld -r` (a
# relocatable link - proves the linker accepts the target's ABS64 relocations
# without needing a target crt/libc), and asserts the result is the right
# arch with hl_app_entries intact. This is the object-level half of
# cross-compilation; a runnable cross-built app additionally needs the target
# crt/libc + the target libhull_platform.a (Tiers B+3, docs/toolchain_free_build.md).
#
# Usage: sh tests/e2e_cross_build.sh   /   make e2e-cross-build
# Requires: a host cc (to build the emit harness), an lld (ld.lld), llvm-nm/nm.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
PASS=0; FAIL=0; WORKDIR=""
cleanup() { [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ] && rm -rf "$WORKDIR"; }
trap cleanup EXIT INT TERM
assert() { m="$1"; shift; if "$@"; then echo "  ok  $m"; PASS=$((PASS+1)); else echo "  FAIL $m"; FAIL=$((FAIL+1)); fi; }

# Resolve an ELF-capable lld (ld.lld).
LLD=""
for c in ld.lld; do p=$(command -v "$c" 2>/dev/null) && LLD="$p"; done
[ -z "$LLD" ] && for d in /opt/homebrew/opt/lld/bin /usr/lib/llvm-*/bin /usr/local/opt/lld/bin; do
    [ -x "$d/ld.lld" ] && LLD="$d/ld.lld"
done
[ -z "$LLD" ] && { echo "SKIP: no ld.lld found (brew install lld / apt install lld)"; exit 0; }

NM="$(command -v llvm-nm 2>/dev/null || command -v nm 2>/dev/null || true)"
[ -z "$NM" ] && { echo "SKIP: no nm found"; exit 0; }

WORKDIR="$(mktemp -d)"
echo "── host: $(uname -m) $(uname -s); cross-linker: $LLD ──"

# Build a tiny harness that emits app_registry.o for a chosen ELF arch.
cat > "$WORKDIR/emit.c" <<'EOF'
#include "hull/obj_emit.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    HlObjArch a = argv[2][0] == 'a' ? HL_OBJ_AARCH64 : HL_OBJ_X86_64;
    HlEmitEntry e[] = {
        { "./app", (const unsigned char *)"x", 1 },
        { "migrations/001.sql", (const unsigned char *)"CREATE TABLE t(x);", 18 },
    };
    HlObjTarget t = { HL_OBJ_ELF, a, 0, 0 };
    unsigned char *o = 0; size_t n = 0;
    if (hl_obj_emit_app_registry(&t, e, 2, &o, &n)) { fprintf(stderr, "emit fail\n"); return 1; }
    FILE *f = fopen(argv[1], "wb"); fwrite(o, 1, n, f); fclose(f); free(o); (void)argc; return 0;
}
EOF
cc -std=c11 -I"$SRCDIR/include" "$WORKDIR/emit.c" "$SRCDIR/src/hull/obj_emit.c" -o "$WORKDIR/emit" 2>/dev/null \
    || { echo "SKIP: could not build the emit harness"; exit 0; }

# arch name -> (emit selector, ld.lld -m emulation, file(1) substring)
for spec in "x86_64:x:elf_x86_64:x86-64" "aarch64:a:aarch64linux:aarch64"; do
    arch="${spec%%:*}"; rest="${spec#*:}"
    sel="${rest%%:*}"; rest="${rest#*:}"
    emu="${rest%%:*}"; want="${rest##*:}"

    "$WORKDIR/emit" "$WORKDIR/reg_$arch.o" "$sel"
    assert "$arch: emitted app_registry.o is $want" sh -c "file '$WORKDIR/reg_$arch.o' | grep -q '$want'"
    # Cross-link (relocatable) with lld for the target arch.
    "$LLD" -r -m "$emu" "$WORKDIR/reg_$arch.o" -o "$WORKDIR/link_$arch.o" 2>"$WORKDIR/lld_$arch.err"
    assert "$arch: ld.lld cross-links the object" [ -f "$WORKDIR/link_$arch.o" ]
    assert "$arch: linked output is $want" sh -c "file '$WORKDIR/link_$arch.o' | grep -q '$want'"
    assert "$arch: hl_app_entries survives the cross-link" sh -c "$NM '$WORKDIR/link_$arch.o' 2>/dev/null | grep -q hl_app_entries"
done

echo ""
echo "cross-build (emit + cross-link) e2e: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
