#!/bin/sh
# check_hardening.sh — post-build hardening verifier for Hull binaries.
#
# Inspects an ELF (or Mach-O / APE) binary and reports which compiler/
# linker hardening properties are present. Uses readelf / otool /
# checksec when available; falls back to byte-level header sniffing.
#
# Usage:
#   scripts/check_hardening.sh [BINARY]
#
# Default BINARY is build/hull. Returns 0 if every REQUIRED check
# passes for the target's platform; 1 otherwise. Properties that the
# host platform can't enforce print "skip" and do not affect the
# exit code.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

BIN="${1:-build/hull}"

if [ ! -f "$BIN" ]; then
    printf 'error: binary not found: %s\n' "$BIN" >&2
    exit 2
fi

# Identify the file format from magic bytes. APE files start with
# the literal "MZ" + a shell prologue; the script segment is detectable
# by reading the first 8 bytes.
fmt=unknown
magic=$(od -An -c -N4 "$BIN" 2>/dev/null | tr -d ' ' || true)
case "$magic" in
    *177ELF*)   fmt=elf  ;;
    MZ*)        fmt=ape  ;;
    *cffaedfe*|*cefaedfe*|*feedface*|*feedfacf*) fmt=macho ;;
esac

# If `file` is available it's more authoritative.
if command -v file >/dev/null 2>&1; then
    desc=$(file -b "$BIN" 2>/dev/null || true)
    case "$desc" in
        *ELF*)      fmt=elf ;;
        *Mach-O*)   fmt=macho ;;
    esac
fi

printf 'check_hardening.sh: %s (format=%s)\n' "$BIN" "$fmt"
printf -- '----------------------------------------------------------\n'

# Status tracking. Each report() call prints one line and updates
# counters. Required-failure (REQ) flips the exit code.
PASS=0
FAIL=0
SKIP=0
EXIT=0

report() {
    # $1 = result (pass|fail|skip|warn)
    # $2 = check name
    # $3 = detail
    case "$1" in
        pass)   printf '  [PASS] %-26s %s\n' "$2" "$3"; PASS=$((PASS+1)) ;;
        fail)   printf '  [FAIL] %-26s %s\n' "$2" "$3"; FAIL=$((FAIL+1)); EXIT=1 ;;
        skip)   printf '  [SKIP] %-26s %s\n' "$2" "$3"; SKIP=$((SKIP+1)) ;;
        warn)   printf '  [WARN] %-26s %s\n' "$2" "$3"; ;;
    esac
}

# ── ELF inspection ────────────────────────────────────────────────────
check_elf() {
    if ! command -v readelf >/dev/null 2>&1; then
        report skip "readelf"             "tool not available; install binutils to verify"
        return
    fi

    hdr=$(readelf -hd "$BIN" 2>/dev/null || true)
    dyn=$(readelf -d  "$BIN" 2>/dev/null || true)
    seg=$(readelf -lW "$BIN" 2>/dev/null || true)
    syms=$(readelf -s "$BIN" 2>/dev/null || true)
    notes=$(readelf -nW "$BIN" 2>/dev/null || true)

    # PIE: ELF type DYN + DT_FLAGS_1 PIE (or absence of EXEC + presence of
    # PT_PHDR). The canonical signal is "Type: DYN" + a PT_INTERP segment.
    if echo "$hdr" | grep -qE 'Type:[[:space:]]+DYN'; then
        report pass "PIE / ASLR"          "ELF type DYN"
    else
        report fail "PIE / ASLR"          "ELF is not DYN (not position-independent)"
    fi

    # RELRO: PT_GNU_RELRO segment present.
    if echo "$seg" | grep -q 'GNU_RELRO'; then
        report pass "RELRO"               "PT_GNU_RELRO present"
    else
        report fail "RELRO"               "no PT_GNU_RELRO segment"
    fi

    # BIND_NOW: DT_FLAGS contains BIND_NOW, or DT_FLAGS_1 contains NOW.
    if echo "$dyn" | grep -qE 'BIND_NOW|FLAGS_1.* NOW'; then
        report pass "BIND_NOW (full RELRO)" "DT_BIND_NOW / DT_FLAGS_1 NOW set"
    else
        report fail "BIND_NOW (full RELRO)" "missing — partial RELRO only"
    fi

    # Non-executable stack: PT_GNU_STACK with no X flag.
    nx=$(echo "$seg" | awk '/GNU_STACK/ {print $7}')
    if [ -n "$nx" ] && echo "$nx" | grep -qv 'E'; then
        report pass "NX stack"            "PT_GNU_STACK flags = $nx"
    elif echo "$seg" | grep -q 'GNU_STACK'; then
        report fail "NX stack"            "PT_GNU_STACK is executable (flags = $nx)"
    else
        report warn "NX stack"            "no PT_GNU_STACK segment (kernel defaults apply)"
    fi

    # Stack canaries: __stack_chk_fail / __stack_chk_guard referenced.
    if echo "$syms" | grep -qE '__stack_chk_fail|__stack_chk_guard'; then
        report pass "stack canaries"      "__stack_chk_fail referenced"
    else
        report fail "stack canaries"      "no __stack_chk_fail reference found"
    fi

    # FORTIFY_SOURCE: any *_chk symbol presence (strcpy_chk, memcpy_chk…).
    if echo "$syms" | grep -qE '_chk@'; then
        n=$(echo "$syms" | grep -cE '_chk@' || true)
        report pass "_FORTIFY_SOURCE"     "$n fortified libc call(s) linked"
    else
        report warn "_FORTIFY_SOURCE"     "no *_chk symbols (debug build or zero overflowable calls)"
    fi

    # CET / IBT: NT_GNU_PROPERTY note with x86 feature 1 IBT/SHSTK.
    if echo "$notes" | grep -qE 'IBT|SHSTK|x86 feature'; then
        report pass "CET (IBT/SHSTK)"     "GNU property note present"
    elif uname -m | grep -qE 'x86_64|amd64'; then
        report warn "CET (IBT/SHSTK)"     "no GNU property note (build without -fcf-protection=full?)"
    else
        report skip "CET (IBT/SHSTK)"     "not an x86_64 build"
    fi

    # ARM64 branch protection: NT_GNU_PROPERTY with AArch64 feature 1 BTI/PAC.
    if uname -m | grep -qE 'aarch64|arm64'; then
        if echo "$notes" | grep -qE 'BTI|PAC|AArch64 feature'; then
            report pass "BTI / PAC"       "GNU property note present"
        else
            report warn "BTI / PAC"       "no AArch64 GNU property note"
        fi
    fi

    # PT_LOAD permissions: no writable+executable segment.
    if echo "$seg" | awk '/LOAD/ {print $7}' | grep -qE 'WE|RWE'; then
        report fail "W^X"                 "found writable+executable LOAD segment (RWX)"
    else
        report pass "W^X"                 "no writable+executable LOAD segments"
    fi

    # RPATH / RUNPATH: presence of either is a runtime-loader attack surface.
    if echo "$dyn" | grep -qE '\(RPATH\)|\(RUNPATH\)'; then
        report warn "RPATH/RUNPATH"       "binary embeds a runtime library path"
    else
        report pass "RPATH/RUNPATH"       "none embedded"
    fi
}

# ── Mach-O inspection ─────────────────────────────────────────────────
check_macho() {
    if ! command -v otool >/dev/null 2>&1; then
        report skip "otool"               "tool not available"
        return
    fi
    hdr=$(otool -hv "$BIN" 2>/dev/null || true)
    flags=$(echo "$hdr" | awk '/MH_MAGIC/ {ok=1} ok && /[A-Z]+/ {for(i=1;i<=NF;i++) print $i}' | tr '\n' ' ')

    # PIE on Mach-O: MH_PIE flag in the header.
    if echo "$hdr" | grep -q 'PIE'; then
        report pass "PIE / ASLR"          "MH_PIE set"
    else
        report fail "PIE / ASLR"          "MH_PIE not set"
    fi

    # No writable+executable section.
    sect=$(otool -lv "$BIN" 2>/dev/null || true)
    if echo "$sect" | awk '/initprot/ {print $2}' | grep -qE 'rwx'; then
        report fail "W^X"                 "found rwx section"
    else
        report pass "W^X"                 "no rwx sections"
    fi

    # Stack canaries: presence of ___stack_chk_fail or ___stack_chk_guard
    # in the symbol table.
    syms=$(nm "$BIN" 2>/dev/null || true)
    if echo "$syms" | grep -qE '_stack_chk_fail|_stack_chk_guard'; then
        report pass "stack canaries"      "__stack_chk_fail referenced"
    else
        report fail "stack canaries"      "no __stack_chk_fail reference"
    fi

    # Hardened Runtime / code signing — Apple-side runtime hardening.
    # `codesign -dv` reports flags; we just check for its presence.
    if command -v codesign >/dev/null 2>&1; then
        cs=$(codesign -dv --verbose=1 "$BIN" 2>&1 || true)
        if echo "$cs" | grep -q 'flags=.*runtime'; then
            report pass "Hardened Runtime" "codesign reports runtime flag"
        elif echo "$cs" | grep -q 'not signed'; then
            report warn "Hardened Runtime" "binary not codesigned (dev build)"
        else
            report warn "Hardened Runtime" "codesigned but no runtime flag"
        fi
    fi

    # Mach-O doesn't have an analogue of RELRO/BIND_NOW (dyld behaves
    # differently); FORTIFY/CET notes are ELF-only.
    report skip "RELRO"                   "Mach-O (dyld provides equivalent)"
    report skip "_FORTIFY_SOURCE check"   "Mach-O symbol naming differs"
    report skip "CET (IBT/SHSTK)"         "Mach-O does not carry the note"
}

# ── APE (Cosmopolitan) inspection ────────────────────────────────────
check_ape() {
    # APE is a hybrid MZ/ELF/Mach-O file. Most ELF-specific hardening
    # is inapplicable by design — Cosmopolitan has its own portable
    # bootloader. Report what we can and skip the rest with the reason.
    report skip "PIE / ASLR"              "APE — portable position-independent by construction"
    report skip "RELRO / BIND_NOW"        "APE — no GNU dynamic linker"
    report skip "stack canaries"          "cosmocc disables by default"
    report skip "_FORTIFY_SOURCE"         "APE — cosmocc does not enable"
    report skip "CET / BTI"               "APE — runs on pre-CET CPUs by design"
    report skip "W^X"                     "APE bootloader handles segment mapping"
}

case "$fmt" in
    elf)    check_elf ;;
    macho)  check_macho ;;
    ape)    check_ape ;;
    *)      printf 'unknown format; cannot verify\n' >&2; exit 2 ;;
esac

printf -- '----------------------------------------------------------\n'
printf 'summary: %d pass, %d fail, %d skip\n' "$PASS" "$FAIL" "$SKIP"

if [ "$EXIT" -ne 0 ]; then
    printf 'verdict: FAIL — required hardening missing\n' >&2
else
    printf 'verdict: OK\n'
fi
exit "$EXIT"
