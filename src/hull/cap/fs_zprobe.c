/*
 * fs_zprobe.c — THROWAWAY MSan layout-perturbation control probe.
 *
 * NOT resolver code and NOT shipped. This exists only on the throwaway branch
 * investigate/msan-wamr-c1 to test the checkpoint-1 hypothesis that the MSan
 * failure in vendored WAMR (wasm_native_lookup_quick_aot_entry) is surfaced by
 * BINARY-LAYOUT PERTURBATION from adding a new cap translation unit, not by the
 * fs resolver's logic. It adds a linked cap TU of comparable size + link
 * placement to cap/fs_resolve.o but with zero filesystem behavior. If an MSan run
 * of (clean main + this probe) reproduces the same WAMR uninit, layout-exposure
 * is proven. Delete after the investigation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>
#include <stddef.h>

/* A spread of external-linkage arithmetic leaves — no I/O, no globals with
 * state, deterministic. Comparable code size to the resolver's walk. */
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return x; }

uint64_t hl_zprobe_00(uint64_t a) { return mix(a + 0x00) ^ (a << 1); }
uint64_t hl_zprobe_01(uint64_t a) { return mix(a + 0x01) ^ (a << 2); }
uint64_t hl_zprobe_02(uint64_t a) { return mix(a + 0x02) ^ (a << 3); }
uint64_t hl_zprobe_03(uint64_t a) { return mix(a + 0x03) ^ (a << 4); }
uint64_t hl_zprobe_04(uint64_t a) { return mix(a + 0x04) ^ (a << 5); }
uint64_t hl_zprobe_05(uint64_t a) { return mix(a + 0x05) ^ (a << 6); }
uint64_t hl_zprobe_06(uint64_t a) { return mix(a + 0x06) ^ (a << 7); }
uint64_t hl_zprobe_07(uint64_t a) { return mix(a + 0x07) ^ (a << 8); }
uint64_t hl_zprobe_08(uint64_t a) { return mix(a + 0x08) ^ (a << 9); }
uint64_t hl_zprobe_09(uint64_t a) { return mix(a + 0x09) ^ (a << 10); }
uint64_t hl_zprobe_10(uint64_t a) { return mix(a + 0x10) ^ (a << 11); }
uint64_t hl_zprobe_11(uint64_t a) { return mix(a + 0x11) ^ (a << 12); }
uint64_t hl_zprobe_12(uint64_t a) { return mix(a + 0x12) ^ (a << 13); }
uint64_t hl_zprobe_13(uint64_t a) { return mix(a + 0x13) ^ (a << 14); }
uint64_t hl_zprobe_14(uint64_t a) { return mix(a + 0x14) ^ (a << 15); }
uint64_t hl_zprobe_15(uint64_t a) { return mix(a + 0x15) ^ (a << 16); }
uint64_t hl_zprobe_16(uint64_t a) { return mix(a + 0x16) ^ (a << 17); }
uint64_t hl_zprobe_17(uint64_t a) { return mix(a + 0x17) ^ (a << 18); }
uint64_t hl_zprobe_18(uint64_t a) { return mix(a + 0x18) ^ (a << 19); }
uint64_t hl_zprobe_19(uint64_t a) { return mix(a + 0x19) ^ (a << 20); }
uint64_t hl_zprobe_20(uint64_t a) { return mix(a + 0x20) ^ (a << 21); }
uint64_t hl_zprobe_21(uint64_t a) { return mix(a + 0x21) ^ (a << 22); }
uint64_t hl_zprobe_22(uint64_t a) { return mix(a + 0x22) ^ (a << 23); }
uint64_t hl_zprobe_23(uint64_t a) { return mix(a + 0x23) ^ (a << 24); }
uint64_t hl_zprobe_24(uint64_t a) { return mix(a + 0x24) ^ (a << 25); }
uint64_t hl_zprobe_25(uint64_t a) { return mix(a + 0x25) ^ (a << 26); }
uint64_t hl_zprobe_26(uint64_t a) { return mix(a + 0x26) ^ (a << 27); }
uint64_t hl_zprobe_27(uint64_t a) { return mix(a + 0x27) ^ (a << 28); }
uint64_t hl_zprobe_28(uint64_t a) { return mix(a + 0x28) ^ (a << 29); }
uint64_t hl_zprobe_29(uint64_t a) { return mix(a + 0x29) ^ (a << 30); }

typedef uint64_t (*hl_zprobe_fn)(uint64_t);

/* An exported const table referencing every leaf keeps them in the link even
 * under --gc-sections, so the TU genuinely perturbs layout. */
const hl_zprobe_fn hl_zprobe_table[] = {
    hl_zprobe_00, hl_zprobe_01, hl_zprobe_02, hl_zprobe_03, hl_zprobe_04,
    hl_zprobe_05, hl_zprobe_06, hl_zprobe_07, hl_zprobe_08, hl_zprobe_09,
    hl_zprobe_10, hl_zprobe_11, hl_zprobe_12, hl_zprobe_13, hl_zprobe_14,
    hl_zprobe_15, hl_zprobe_16, hl_zprobe_17, hl_zprobe_18, hl_zprobe_19,
    hl_zprobe_20, hl_zprobe_21, hl_zprobe_22, hl_zprobe_23, hl_zprobe_24,
    hl_zprobe_25, hl_zprobe_26, hl_zprobe_27, hl_zprobe_28, hl_zprobe_29,
};
const size_t hl_zprobe_table_len = sizeof(hl_zprobe_table) / sizeof(hl_zprobe_table[0]);

uint64_t hl_zprobe_run(uint64_t seed)
{
    uint64_t a = seed;
    for (size_t i = 0; i < hl_zprobe_table_len; i++)
        a = hl_zprobe_table[i](a);
    return a;
}
