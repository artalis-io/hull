/*
 * test_span_sdk.c — native unit tests for the guest SDK header templates/hull_span.h
 * (mapped-spans checkpoint 3b). Exercises the natively-testable surface: the
 * locked wire decoder (hull_span_decode), name lookup (hull_span_find), and the
 * scratch-address narrowing guard (hull_span__narrow) that rejects a destination
 * >= 4 GiB before it can be truncated into the (i32,i32,i32) host_call ABI.
 *
 * hull_span_setup's full count+cbSize query loop drives a real host_call and is
 * wasm32-only (a 64-bit native scratch is >= 4 GiB and is rejected by design);
 * that path is covered end-to-end by the WASM e2e + the differential test. Here
 * we assert the ABI-limit rejection and the decoder it wires together.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

/* Mock host_call so hull_span_setup links. On a 64-bit host the setup scratch is
 * >= 4 GiB and is rejected before this is ever called; it returns an empty set so
 * a (hypothetical) low-address host stays harmless. */
static int32_t host_call(int32_t op, int32_t ptr, int32_t idx)
{
    (void)op; (void)ptr; (void)idx;
    return 0; /* count = 0 */
}

#include "hull_span.h"
#include "utest.h"

/* Build a wire HlSpanMetaV1 record into rec[96]. */
static void build_rec(uint8_t rec[96], uint16_t version, uint16_t struct_size,
                      uint32_t flags, const char *name,
                      uint64_t base, uint64_t len, uint64_t foffset)
{
    memset(rec, 0, 96);
    rec[HULL_SPAN_OFF_VERSION]      = (uint8_t)(version & 0xff);
    rec[HULL_SPAN_OFF_VERSION + 1]  = (uint8_t)(version >> 8);
    rec[HULL_SPAN_OFF_STRUCTSZ]     = (uint8_t)(struct_size & 0xff);
    rec[HULL_SPAN_OFF_STRUCTSZ + 1] = (uint8_t)(struct_size >> 8);
    for (int i = 0; i < 4; i++) rec[HULL_SPAN_OFF_FLAGS + i] = (uint8_t)((flags >> (8 * i)) & 0xff);
    if (name) { size_t n = strlen(name); if (n > 63) n = 63; memcpy(rec + HULL_SPAN_OFF_NAME, name, n); }
    for (int i = 0; i < 8; i++) rec[HULL_SPAN_OFF_BASE + i]    = (uint8_t)((base >> (8 * i)) & 0xff);
    for (int i = 0; i < 8; i++) rec[HULL_SPAN_OFF_LEN + i]     = (uint8_t)((len >> (8 * i)) & 0xff);
    for (int i = 0; i < 8; i++) rec[HULL_SPAN_OFF_FOFFSET + i] = (uint8_t)((foffset >> (8 * i)) & 0xff);
}

/* ── decode: full v1 record, every field, little-endian ─────────────────────── */
UTEST(span_sdk, decode_full_record)
{
    uint8_t rec[96];
    build_rec(rec, 1, 96, HULL_SPAN_FLAG_RO, "source",
              0x0011223344556677ULL, 4096, 8195);
    HullSpan s;
    ASSERT_EQ(hull_span_decode(rec, 96, &s), 0);
    ASSERT_EQ((unsigned)s.flags, (unsigned)HULL_SPAN_FLAG_RO);
    ASSERT_STREQ(s.name, "source");
    ASSERT_TRUE(s.base == 0x0011223344556677ULL);   /* full 64-bit LE decode */
    ASSERT_TRUE(s.len == 4096ULL);
    ASSERT_TRUE(s.foffset == 8195ULL);
}

/* ── decode: name exactly 63 chars is preserved + NUL-terminated at [63] ─────── */
UTEST(span_sdk, decode_name_terminated)
{
    char long_name[64];
    for (int i = 0; i < 63; i++) long_name[i] = 'a';
    long_name[63] = '\0';
    uint8_t rec[96];
    build_rec(rec, 1, 96, 0, long_name, 0, 0, 0);
    /* wipe the wire NUL so the record's name field is a full 64 non-NUL bytes;
     * decode must still terminate at index 63. */
    rec[HULL_SPAN_OFF_NAME + 63] = 'z';
    HullSpan s;
    ASSERT_EQ(hull_span_decode(rec, 96, &s), 0);
    ASSERT_EQ((int)strlen(s.name), 63);
    ASSERT_EQ((int)s.name[63], 0);
}

/* ── decode: short record (< 96 bytes written) is rejected ──────────────────── */
UTEST(span_sdk, decode_short_record)
{
    uint8_t rec[96];
    build_rec(rec, 1, 96, 0, "x", 1, 2, 3);
    HullSpan s;
    ASSERT_EQ(hull_span_decode(rec, 95, &s), HULL_SPAN_ERR_VERSION);
    ASSERT_EQ(hull_span_decode(rec, 8, &s), HULL_SPAN_ERR_VERSION);
    ASSERT_EQ(hull_span_decode(rec, 0, &s), HULL_SPAN_ERR_VERSION);
}

/* ── decode: version / struct_size gating ──────────────────────────────────── */
UTEST(span_sdk, decode_version_gate)
{
    uint8_t rec[96];
    HullSpan s;
    build_rec(rec, 2, 96, 0, "x", 0, 0, 0);   /* wrong version */
    ASSERT_EQ(hull_span_decode(rec, 96, &s), HULL_SPAN_ERR_VERSION);
    build_rec(rec, 0, 96, 0, "x", 0, 0, 0);   /* version 0 */
    ASSERT_EQ(hull_span_decode(rec, 96, &s), HULL_SPAN_ERR_VERSION);
    build_rec(rec, 1, 95, 0, "x", 0, 0, 0);   /* struct_size < v1 */
    ASSERT_EQ(hull_span_decode(rec, 96, &s), HULL_SPAN_ERR_VERSION);
}

/* ── decode: forward-compat — a newer host (struct_size 128) with a v1 prefix
 *    written (rec_len 96) decodes the v1 fields fine ────────────────────────── */
UTEST(span_sdk, decode_forward_compat)
{
    uint8_t rec[96];
    build_rec(rec, 1, 128, HULL_SPAN_FLAG_RO, "src", 0xDEAD, 64, 0);
    HullSpan s;
    ASSERT_EQ(hull_span_decode(rec, 96, &s), 0);
    ASSERT_STREQ(s.name, "src");
    ASSERT_TRUE(s.base == 0xDEADULL);
    ASSERT_TRUE(s.len == 64ULL);
}

/* ── find: deterministic linear lookup, declaration order, unknown -> -1 ─────── */
UTEST(span_sdk, find_by_name)
{
    HullSpan spans[3];
    memset(spans, 0, sizeof(spans));
    strcpy(spans[0].name, "graph");
    strcpy(spans[1].name, "landmarks");
    strcpy(spans[2].name, "grid");
    ASSERT_EQ(hull_span_find(spans, 3, "graph"), 0);
    ASSERT_EQ(hull_span_find(spans, 3, "landmarks"), 1);
    ASSERT_EQ(hull_span_find(spans, 3, "grid"), 2);
    ASSERT_EQ(hull_span_find(spans, 3, "nope"), -1);
    ASSERT_EQ(hull_span_find(spans, 3, "gr"), -1);       /* prefix is not a match */
    ASSERT_EQ(hull_span_find(spans, 3, "grids"), -1);    /* superstring is not a match */
    ASSERT_EQ(hull_span_find(spans, 0, "graph"), -1);    /* empty set */
}

/* ── narrow: the > UINT32_MAX scratch rejection (Memory64 / 64-bit native) ───── */
UTEST(span_sdk, narrow_guard)
{
    hull_span_i32 out = 0;
    ASSERT_EQ(hull_span__narrow((hull_span_uptr)0x1000u, &out), 0);
    ASSERT_EQ((uint32_t)out, 0x1000u);
    ASSERT_EQ(hull_span__narrow((hull_span_uptr)0xFFFFFFFFu, &out), 0);   /* exactly 4 GiB - 1: ok */
    ASSERT_EQ((uint32_t)out, 0xFFFFFFFFu);
    if (sizeof(void *) > 4) {
        ASSERT_EQ(hull_span__narrow((hull_span_uptr)0x100000000ULL, &out),
                  HULL_SPAN_ERR_ADDR);                                    /* 4 GiB: reject */
        ASSERT_EQ(hull_span__narrow((hull_span_uptr)0xDEADBEEF00ULL, &out),
                  HULL_SPAN_ERR_ADDR);                                    /* high: reject */
    }
}

/* ── setup: on a 64-bit host the scratch record is >= 4 GiB, so setup rejects
 *    via the ABI guard before any host_call (documents the wasm32-only limit).
 *    On the (unlikely) low-scratch case the mock returns count 0. ───────────── */
UTEST(span_sdk, setup_rejects_high_scratch)
{
    HullSpan out[HULL_SPAN_MAX];
    int r = hull_span_setup(out, HULL_SPAN_MAX);
    if (sizeof(void *) > 4)
        ASSERT_EQ(r, HULL_SPAN_ERR_ADDR);
    else
        ASSERT_EQ(r, 0);
}

UTEST_MAIN()
