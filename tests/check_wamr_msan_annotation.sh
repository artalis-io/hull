#!/bin/sh
# check_wamr_msan_annotation.sh — permanent fixture for WAMR patch 0006.
#
# Patch 0006 annotates a MemorySanitizer SHADOW-GAP false positive at the
# boundary between intentionally-uninstrumented WAMR and MSan's always-on strcmp
# interceptor (see docs/wamr_patches.md "Patch 0006"). This fixture locks the
# policy the annotation depends on so a silent regression fails loudly, asserting
# (against the ACTUAL compile commands + object, not Makefile comments):
#
#   1. Under MSAN, the WAMR compile of wasm_native.o carries -DHL_MSAN.
#   2. That same command does NOT carry -fsanitize=memory (WAMR stays
#      intentionally uninstrumented; the annotation is the whole point).
#   3. A normal build carries neither -DHL_MSAN nor a compiled __msan_unpoison
#      reference in wasm_native.o.
#   4. The annotation un-poisons ONLY the signature buffer and sits immediately
#      before the intercepted comparison (key.signature = signature; -> bsearch).
#   5. Docs frame it as a shadow-gap boundary annotation, not a memory-safety fix.
#
# Uses `make -n` (no compiler is invoked for the command-shape assertions), so it
# runs on any host; the normal-build symbol check builds one object.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
OBJ=build/wamr_core/iwasm/common/wasm_native.o
SRC=build/wamr-patched/core/iwasm/common/wasm_native.c
# CC only appears verbatim in the `make -n` command text for the -DHL_MSAN /
# -fsanitize=memory assertions (no compiler is invoked); the symbol check builds one
# WAMR object with whatever CC is available. Default to cc for portability.
CC=${CC:-cc}
fail() { echo "check-wamr-msan-annotation FAIL: $*" >&2; exit 1; }

# Ensure the patched WAMR tree (with 0006) exists.
sh scripts/wamr_apply_patches.sh >/dev/null 2>&1 || fail "wamr patch apply failed"
[ -f "$SRC" ] || fail "staged WAMR source missing: $SRC"

# The compile recipe is "... -c -o <OBJ> <SRC>", so the object is followed by the
# source path (not end-of-line) — match "-o <OBJ> " space-delimited, not anchored.
cmd_for() { make CC="$CC" "$@" -Bn "$OBJ" 2>/dev/null | grep -F -- " -o $OBJ " | tail -1; }

# 1 + 2. MSAN build: -DHL_MSAN present, -fsanitize=memory absent.
msan_cmd=$(cmd_for MSAN=1)
[ -n "$msan_cmd" ] || fail "no MSAN compile command for $OBJ"
echo "$msan_cmd" | grep -q -- '-DHL_MSAN' \
    || fail "MSAN WAMR compile of wasm_native.o lacks -DHL_MSAN (annotation would compile out)"
echo "$msan_cmd" | grep -q -- '-fsanitize=memory' \
    && fail "WAMR wasm_native.o is -fsanitize=memory-instrumented under MSAN — policy changed; re-assess the annotation"

# 3. Normal build: no -DHL_MSAN in the command, no __msan_unpoison in the object.
norm_cmd=$(cmd_for)
[ -n "$norm_cmd" ] || fail "no normal compile command for $OBJ"
echo "$norm_cmd" | grep -q -- '-DHL_MSAN' \
    && fail "normal WAMR compile leaks -DHL_MSAN"
make CC="$CC" "$OBJ" >/dev/null 2>&1 || fail "normal build of $OBJ failed"
nm "$OBJ" 2>/dev/null | grep -q '__msan_unpoison' \
    && fail "normal build of wasm_native.o references __msan_unpoison"

# 4. Annotation scope + placement (source-level, in the staged tree).
grep -q '__msan_unpoison(signature, sizeof(signature))' "$SRC" \
    || fail "the signature-buffer un-poison is missing from $SRC"
# no un-poison of any buffer other than signature
if grep -E '__msan_unpoison\(' "$SRC" | grep -vq 'signature, sizeof(signature)'; then
    fail "an __msan_unpoison targets a buffer other than the signature buffer"
fi
# immediately before the intercepted comparison: only a closing #endif may sit
# between the un-poison and 'key.signature = signature;'.
u=$(grep -n '__msan_unpoison(signature, sizeof(signature))' "$SRC" | head -1 | cut -d: -f1)
k=$(awk -v u="$u" 'NR>u && /key\.signature = signature;/ {print NR; exit}' "$SRC")
[ -n "$k" ] || fail "no 'key.signature = signature;' after the un-poison"
gap=$((k - u))
[ "$gap" -ge 1 ] && [ "$gap" -le 2 ] \
    || fail "un-poison is not immediately before key.signature=signature (gap=$gap lines)"
between=$(awk -v u="$u" -v k="$k" 'NR>u && NR<k' "$SRC" | grep -vE '^[[:space:]]*(#endif)?[[:space:]]*$' || true)
[ -z "$between" ] || fail "unexpected code between the un-poison and the comparison: $between"

# 5. Documentation frames it correctly.
D=docs/wamr_patches.md
grep -qiE 'shadow.gap' "$D" || fail "$D does not describe a shadow-gap annotation"
grep -qiE 'NOT a C uninitialized-read defect|false positive' "$D" \
    || fail "$D does not state this is a false positive / not a memory-safety defect"

echo "OK: WAMR MSan shadow-gap annotation policy holds"
echo "  - MSAN wasm_native.o compile carries -DHL_MSAN, not -fsanitize=memory"
echo "  - normal build carries neither -DHL_MSAN nor an __msan_unpoison reference"
echo "  - annotation un-poisons only 'signature', immediately before the comparison"
echo "  - docs frame it as a shadow-gap boundary annotation"
