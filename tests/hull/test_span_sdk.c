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

/* Mock host_call so hull_span_setup links, with an invocation counter so a test
 * can prove the count-only query issues exactly one host call (the count query)
 * and no record queries. The count query (idx == -1) reports 3 attached spans; a
 * record query (idx >= 0) reports an empty record (unreached on a 64-bit host,
 * where the scratch narrow rejects before any record query). */
static int g_hostcalls = 0;
static int32_t host_call(int32_t op, int32_t ptr, int32_t idx)
{
    (void)op; (void)ptr;
    g_hostcalls++;
    if (idx == -1) return 3;   /* count query -> 3 spans */
    return 0;                  /* record query -> empty (low-address host only) */
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

/* ── setup argument validation: rejected BEFORE any host call. ─────────────── */
UTEST(span_sdk, setup_rejects_null_dest_positive_cap)
{
    /* out == NULL with a positive capacity would write through NULL — reject,
     * without issuing any host call. */
    g_hostcalls = 0;
    ASSERT_EQ(hull_span_setup((HullSpan *)0, 5), HULL_SPAN_ERR_ARG);
    ASSERT_EQ(g_hostcalls, 0);
}

UTEST(span_sdk, setup_rejects_negative_cap)
{
    HullSpan out[HULL_SPAN_MAX];
    g_hostcalls = 0;
    ASSERT_EQ(hull_span_setup(out, -1), HULL_SPAN_ERR_ARG);
    ASSERT_EQ(hull_span_setup((HullSpan *)0, -1), HULL_SPAN_ERR_ARG);
    ASSERT_EQ(g_hostcalls, 0);
}

/* ── count-only query: NULL + out_cap 0 issues ONLY the count query (no scratch
 *    narrowing) and returns the count EXACTLY, on every target. ─────────────── */
UTEST(span_sdk, setup_null_zero_cap_returns_count)
{
    g_hostcalls = 0;
    int r = hull_span_setup((HullSpan *)0, 0);
    ASSERT_EQ(r, 3);              /* exact mock count */
    ASSERT_EQ(g_hostcalls, 1);    /* exactly one host call: the count query */
}

/* ── record fetch: the scratch record must be narrowed to the i32 ABI. On a
 *    64-bit host the address is >= 4 GiB and is rejected with ERR_ADDR -- AFTER
 *    the count query (g_hostcalls == 1), BEFORE any record query. ───────────── */
UTEST(span_sdk, setup_high_scratch_rejects_after_count)
{
    HullSpan out[HULL_SPAN_MAX];
    g_hostcalls = 0;
    int r = hull_span_setup(out, HULL_SPAN_MAX);
    if (sizeof(void *) > 4) {
        ASSERT_EQ(r, HULL_SPAN_ERR_ADDR);
        ASSERT_EQ(g_hostcalls, 1);   /* count query only; no record query issued */
    } else {
        /* low-address host: record query runs; the mock reports empty -> QUERY */
        ASSERT_EQ(r, HULL_SPAN_ERR_QUERY);
    }
}

UTEST_MAIN()
