/*
 * hull_span.h native-vs-WASM differential test (#324 3b). Runs identical byte
 * fixtures through the SDK's pure decoders on TWO targets — natively (this TU,
 * compiled against templates/hull_span.h) and a real wasm32 guest
 * (tests/fixtures/compute/spandiff.c, same spandiff_ops.h body) on the
 * interpreter AND AOT — and requires byte-for-byte parity. An independent
 * little-endian oracle additionally pins correctness so a shared decode bug
 * (identical on both targets) cannot pass on parity alone.
 *
 * Coverage: rd16/rd32/rd64 across all integer widths, signed values, float bit
 * patterns, and misaligned + exact-end offsets; hull_span_decode metadata
 * (valid, version/struct_size/rec_len rejection = overflow/one-past); the
 * hull_span__narrow 32-bit ABI boundary (exact UINT32_MAX accepted; a native-only
 * one-past-4GiB reject, since wasm32 cannot represent it — the #318 Memory64
 * leg); and hull_span_find name lookup.
 *
 * The guest uses only host_call-free SDK ops, so spandiff.wasm imports nothing
 * and loads in this bare WAMR harness. AOT fixture (gen_spandiff_aot.h) is
 * build-generated when wamrc is present; the AOT sub-cases skip otherwise (the
 * CI AOT job builds wamrc and asserts they do NOT skip).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "utest.h"
#include "wasm_export.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native SDK path: give hull_span.h a host_call stub (only hull_span_setup would
 * use it; the differential ops don't), then include the SAME shared op body the
 * guest compiles. */
static int32_t spandiff_host_call_stub(int32_t a, int32_t b, int32_t c)
{ (void)a; (void)b; (void)c; return -1; }
#define HULL_SPAN_HOST_CALL spandiff_host_call_stub
#include "hull_span.h"         /* -Itemplates */
#include "spandiff_ops.h"      /* -Itests/fixtures/compute */

/* Build-embedded guest bytes: interpreter .wasm always; AOT when wamrc built. */
#include "gen_spandiff_wasm.h" /* spandiff_wasm[] + _len */
#include "gen_spandiff_aot.h"  /* spandiff_aot[]  + _len (or a zero-length stub) */

#define OUT_CAP 128

/* ── WAMR harness ─────────────────────────────────────────────────────────── */
static wasm_module_inst_t g_inst;
static wasm_exec_env_t    g_env;
static wasm_module_t      g_mod;
static uint8_t           *g_buf;

static wasm_module_inst_t load(const uint8_t *b, uint32_t len, wasm_module_t *m,
                               wasm_exec_env_t *e, uint8_t **owned)
{
    char err[192];
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return NULL;
    memcpy(copy, b, len);
    wasm_module_t mod = wasm_runtime_load(copy, len, err, sizeof(err));
    if (!mod) { free(copy); return NULL; }
    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, 64 * 1024, 256 * 1024, err, sizeof(err));
    if (!inst) { wasm_runtime_unload(mod); free(copy); return NULL; }
    *e = wasm_runtime_create_exec_env(inst, 32 * 1024);
    *m = mod; *owned = copy;
    return inst;
}

/* Call hull_process(in, in_len, out, out_max) inside the guest; copy the guest's
 * output back. Returns the guest's int32 return (output length or negative). */
static int run_guest(wasm_module_inst_t inst, wasm_exec_env_t env,
                     const uint8_t *in, uint32_t in_len, uint8_t *out)
{
    void *in_native = NULL, *out_native = NULL;
    uint64_t in_off  = wasm_runtime_module_malloc(inst, in_len ? in_len : 1, &in_native);
    uint64_t out_off = wasm_runtime_module_malloc(inst, OUT_CAP, &out_native);
    if (!in_off || !out_off) {
        if (in_off) wasm_runtime_module_free(inst, in_off);
        if (out_off) wasm_runtime_module_free(inst, out_off);
        return -1000;
    }
    if (in_len) memcpy(in_native, in, in_len);
    memset(out_native, 0, OUT_CAP);
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, "hull_process");
    uint32_t argv[4] = { (uint32_t)in_off, in_len, (uint32_t)out_off, OUT_CAP };
    wasm_runtime_set_exception(inst, NULL);
    int ret;
    if (!wasm_runtime_call_wasm(env, f, 4, argv)) {
        ret = -1001;
    } else {
        ret = (int32_t)argv[0];
        if (ret > 0 && ret <= OUT_CAP) memcpy(out, out_native, (size_t)ret);
    }
    wasm_runtime_module_free(inst, in_off);
    wasm_runtime_module_free(inst, out_off);
    return ret;
}

/* ── little-endian fixture builders ───────────────────────────────────────── */
static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i)); }

/* Build an OP_READ fixture: [op][kind][endian][off u64][len u64][window]. */
static uint32_t mk_read(uint8_t *in, uint8_t kind, uint8_t endian, uint64_t off,
                        uint64_t len, const uint8_t *win, uint32_t win_n)
{
    in[0] = SPANDIFF_OP_READ; in[1] = kind; in[2] = endian;
    put64(in + 3, off); put64(in + 11, len);
    if (win_n) memcpy(in + 19, win, win_n);
    return 19 + win_n;
}
/* Oracle: a successful read returns [status 0][value 8]; the value is the typed
 * result i64/u64-extended, or a float's raw bits. */
static void exp_ok(uint8_t *exp, uint64_t val) { put32(exp, 0); put64(exp + 4, val); }
static void exp_range(uint8_t *exp) { put32(exp, (uint32_t)(int32_t)(-4) /* HULL_SPAN_ERR_RANGE */); }

/* One differential fixture: run native + interp (+AOT) and require parity;
 * when exp_len >= 0, also require the native result matches the oracle bytes. */
static void diff_case(int *utest_result, wasm_exec_env_t aot_env, wasm_module_inst_t aot_inst,
                      const char *label, const uint8_t *in, uint32_t in_len,
                      const uint8_t *exp, int exp_len)
{
    (void)label;
    uint8_t nat[OUT_CAP]; memset(nat, 0, sizeof(nat));
    int nat_len = spandiff_run((const hull_span_u8 *)in, in_len, (hull_span_u8 *)nat, OUT_CAP);

    uint8_t wi[OUT_CAP]; memset(wi, 0, sizeof(wi));
    int wi_len = run_guest(g_inst, g_env, in, in_len, wi);

    /* interpreter parity */
    ASSERT_EQ(nat_len, wi_len);
    if (nat_len > 0) ASSERT_EQ(0, memcmp(nat, wi, (size_t)nat_len));

    /* AOT parity (only when the AOT fixture was built) */
    if (spandiff_aot_len > 0 && aot_inst) {
        uint8_t wa[OUT_CAP]; memset(wa, 0, sizeof(wa));
        int wa_len = run_guest(aot_inst, aot_env, in, in_len, wa);
        ASSERT_EQ(nat_len, wa_len);
        if (nat_len > 0) ASSERT_EQ(0, memcmp(nat, wa, (size_t)nat_len));
    }

    /* independent oracle (correctness, not just cross-target agreement) */
    if (exp_len >= 0) {
        ASSERT_EQ(nat_len, exp_len);
        if (exp_len > 0) ASSERT_EQ(0, memcmp(nat, exp, (size_t)exp_len));
    }
}

/* Build a valid v1 record with the given fields (little-endian, name copied). */
static void build_record(uint8_t rec[96], uint16_t version, uint16_t struct_size,
                         uint32_t flags, const char *name,
                         uint64_t base, uint64_t len, uint64_t foffset)
{
    memset(rec, 0, 96);
    put16(rec + HULL_SPAN_OFF_VERSION, version);
    put16(rec + HULL_SPAN_OFF_STRUCTSZ, struct_size);
    put32(rec + HULL_SPAN_OFF_FLAGS, flags);
    if (name) { size_t n = strlen(name); if (n > 63) n = 63; memcpy(rec + HULL_SPAN_OFF_NAME, name, n); }
    put64(rec + HULL_SPAN_OFF_BASE, base);
    put64(rec + HULL_SPAN_OFF_LEN, len);
    put64(rec + HULL_SPAN_OFF_FOFFSET, foffset);
}

UTEST(hull_span_diff, native_vs_wasm_interp_aot)
{
    RuntimeInitArgs init; memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init));

    g_inst = load(spandiff_wasm, spandiff_wasm_len, &g_mod, &g_env, &g_buf);
    ASSERT_TRUE(g_inst != NULL);

    /* AOT instance (optional) */
    wasm_module_inst_t aot_inst = NULL; wasm_exec_env_t aot_env = NULL;
    wasm_module_t aot_mod = NULL; uint8_t *aot_buf = NULL;
    if (spandiff_aot_len > 0) {
        aot_inst = load(spandiff_aot, spandiff_aot_len, &aot_mod, &aot_env, &aot_buf);
        ASSERT_TRUE(aot_inst != NULL); /* fixture built => must load */
    }

    uint8_t in[256], exp[OUT_CAP];

    /* ── rd16 / rd32 / rd64: integer widths + LE decoding (oracle) ── */
    { uint8_t b[] = { SPANDIFF_OP_RD16, 0, 0x01, 0x02 };
      put64(exp, 0x0201ull); diff_case(utest_result, aot_env, aot_inst, "rd16", b, sizeof(b), exp, 8); }
    { uint8_t b[] = { SPANDIFF_OP_RD32, 0, 0x01, 0x02, 0x03, 0x04 };
      put64(exp, 0x04030201ull); diff_case(utest_result, aot_env, aot_inst, "rd32", b, sizeof(b), exp, 8); }
    { uint8_t b[] = { SPANDIFF_OP_RD64, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
      put64(exp, 0x0807060504030201ull); diff_case(utest_result, aot_env, aot_inst, "rd64", b, sizeof(b), exp, 8); }

    /* ── signed / high-bit values ── */
    { uint8_t b[] = { SPANDIFF_OP_RD32, 0, 0xFF, 0xFF, 0xFF, 0xFF };
      put64(exp, 0xFFFFFFFFull); diff_case(utest_result, aot_env, aot_inst, "rd32_signed", b, sizeof(b), exp, 8); }
    { uint8_t b[] = { SPANDIFF_OP_RD64, 0, 0, 0, 0, 0, 0, 0, 0, 0x80 };
      put64(exp, 0x8000000000000000ull); diff_case(utest_result, aot_env, aot_inst, "rd64_signbit", b, sizeof(b), exp, 8); }

    /* ── float bit patterns (raw bits via rd32/rd64) ── */
    { uint8_t b[10] = { SPANDIFF_OP_RD32, 0 }; put32(b + 2, 0x3F800000u); /* 1.0f */
      put64(exp, 0x3F800000ull); diff_case(utest_result, aot_env, aot_inst, "f32_one", b, 6, exp, 8); }
    { uint8_t b[10] = { SPANDIFF_OP_RD32, 0 }; put32(b + 2, 0x80000000u); /* -0.0f */
      put64(exp, 0x80000000ull); diff_case(utest_result, aot_env, aot_inst, "f32_negzero", b, 6, exp, 8); }
    { uint8_t b[10] = { SPANDIFF_OP_RD32, 0 }; put32(b + 2, 0x7FC00000u); /* NaN */
      put64(exp, 0x7FC00000ull); diff_case(utest_result, aot_env, aot_inst, "f32_nan", b, 6, exp, 8); }
    { uint8_t b[10] = { SPANDIFF_OP_RD64, 0 }; put64(b + 2, 0x7FF0000000000000ull); /* +Inf f64 */
      put64(exp, 0x7FF0000000000000ull); diff_case(utest_result, aot_env, aot_inst, "f64_inf", b, 10, exp, 8); }

    /* ── misalignment: read at odd offsets ── */
    { uint8_t b[] = { SPANDIFF_OP_RD32, 1, 0x00, 0xAA, 0xBB, 0xCC, 0xDD };
      put64(exp, 0xDDCCBBAAull); diff_case(utest_result, aot_env, aot_inst, "rd32_off1", b, sizeof(b), exp, 8); }
    { uint8_t b[] = { SPANDIFF_OP_RD64, 3, 0,0,0, 1,2,3,4,5,6,7,8 };
      put64(exp, 0x0807060504030201ull); diff_case(utest_result, aot_env, aot_inst, "rd64_off3", b, sizeof(b), exp, 8); }

    /* ── exact-end read: last 8 bytes exactly ── */
    { uint8_t b[] = { SPANDIFF_OP_RD64, 4, 9,9,9,9, 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88 };
      put64(exp, 0x8877665544332211ull); diff_case(utest_result, aot_env, aot_inst, "rd64_exact_end", b, sizeof(b), exp, 8); }

    /* ── metadata decode: valid record (oracle = status 0 + fields) ── */
    { uint8_t rec[96]; build_record(rec, 1, 96, HULL_SPAN_FLAG_RO, "source",
                                    0x1122334455667788ull, 4096, 8195);
      in[0] = SPANDIFF_OP_DECODE; put16(in + 1, 96); memcpy(in + 3, rec, 96);
      memset(exp, 0, sizeof(exp));
      put32(exp, 0);                    /* status 0 */
      put32(exp + 4, HULL_SPAN_FLAG_RO);
      put64(exp + 8, 0x1122334455667788ull);
      put64(exp + 16, 4096);
      put64(exp + 24, 8195);
      memcpy(exp + 32, "source", 6);    /* name, rest NUL */
      diff_case(utest_result, aot_env, aot_inst, "decode_valid", in, 3 + 96, exp, 96); }

    /* ── metadata decode: forward-compatible larger struct_size accepted ── */
    { uint8_t rec[96]; build_record(rec, 1, 200, 0, "big", 1, 2, 3);
      in[0] = SPANDIFF_OP_DECODE; put16(in + 1, 96); memcpy(in + 3, rec, 96);
      memset(exp, 0, sizeof(exp));
      put32(exp, 0); put32(exp + 4, 0); put64(exp + 8, 1); put64(exp + 16, 2); put64(exp + 24, 3);
      memcpy(exp + 32, "big", 3);
      diff_case(utest_result, aot_env, aot_inst, "decode_bigstruct", in, 3 + 96, exp, 96); }

    /* ── metadata decode: exact 64-bit base/len/foffset (all > UINT32_MAX) ── */
    { uint8_t rec[96]; build_record(rec, 1, 96, 0, "huge",
                                    0x1122334455667788ull,       /* base    */
                                    0x0000000100000000ull,       /* len = 4 GiB */
                                    0x0000000100000003ull);      /* foffset > 4 GiB */
      in[0] = SPANDIFF_OP_DECODE; put16(in + 1, 96); memcpy(in + 3, rec, 96);
      memset(exp, 0, sizeof(exp));
      put32(exp, 0); put32(exp + 4, 0);
      put64(exp + 8, 0x1122334455667788ull);
      put64(exp + 16, 0x0000000100000000ull);
      put64(exp + 24, 0x0000000100000003ull);
      memcpy(exp + 32, "huge", 4);
      diff_case(utest_result, aot_env, aot_inst, "decode_64bit_fields", in, 3 + 96, exp, 96); }

    /* ── metadata decode: rejection paths (overflow / one-past / bad version) ── */
    { uint8_t rec[96]; build_record(rec, 1, 96, 0, "x", 0, 0, 0);
      in[0] = SPANDIFF_OP_DECODE; put16(in + 1, 95); memcpy(in + 3, rec, 96);  /* rec_len one short */
      put32(exp, (uint32_t)(int32_t)HULL_SPAN_ERR_VERSION);
      diff_case(utest_result, aot_env, aot_inst, "decode_reclen_short", in, 3 + 96, exp, 4); }
    { uint8_t rec[96]; build_record(rec, 2, 96, 0, "x", 0, 0, 0);              /* bad version */
      in[0] = SPANDIFF_OP_DECODE; put16(in + 1, 96); memcpy(in + 3, rec, 96);
      put32(exp, (uint32_t)(int32_t)HULL_SPAN_ERR_VERSION);
      diff_case(utest_result, aot_env, aot_inst, "decode_bad_version", in, 3 + 96, exp, 4); }
    { uint8_t rec[96]; build_record(rec, 1, 95, 0, "x", 0, 0, 0);              /* struct_size < 96 */
      in[0] = SPANDIFF_OP_DECODE; put16(in + 1, 96); memcpy(in + 3, rec, 96);
      put32(exp, (uint32_t)(int32_t)HULL_SPAN_ERR_VERSION);
      diff_case(utest_result, aot_env, aot_inst, "decode_structsz_short", in, 3 + 96, exp, 4); }

    /* ── narrow: 32-bit ABI boundary. p<4GiB agrees on both targets. ── */
    { in[0] = SPANDIFF_OP_NARROW; put64(in + 1, 0); memset(exp, 0, 8);
      put32(exp, 0); put32(exp + 4, 0);
      diff_case(utest_result, aot_env, aot_inst, "narrow_zero", in, 9, exp, 8); }
    { in[0] = SPANDIFF_OP_NARROW; put64(in + 1, 0xFFFFFFFFull);            /* exact UINT32_MAX */
      put32(exp, 0); put32(exp + 4, 0xFFFFFFFFu);
      diff_case(utest_result, aot_env, aot_inst, "narrow_max32", in, 9, exp, 8); }

    /* narrow one-past-4GiB reject: native only. wasm32's uptr is 32-bit so the
     * value truncates and cannot exercise the reject — the #318 Memory64 leg. */
    { in[0] = SPANDIFF_OP_NARROW; put64(in + 1, 0x100000000ull);
      uint8_t nat[OUT_CAP];
      int nl = spandiff_run((const hull_span_u8 *)in, 9, (hull_span_u8 *)nat, OUT_CAP);
      ASSERT_EQ(nl, 4);
      ASSERT_EQ((int32_t)hull_span__rd32((const hull_span_u8 *)nat), HULL_SPAN_ERR_ADDR); }

    /* ── name lookup ── */
    { int count = 3; const char *names[] = { "alpha", "source", "beta" };
      in[0] = SPANDIFF_OP_FIND; in[1] = (uint8_t)count;
      for (int i = 0; i < count; i++) {
          memset(in + 2 + i * 64, 0, 64);
          memcpy(in + 2 + i * 64, names[i], strlen(names[i]));
      }
      uint32_t base = 2 + (uint32_t)count * 64;
      const char *q = "source"; memcpy(in + base, q, strlen(q) + 1);
      put32(exp, 1);
      diff_case(utest_result, aot_env, aot_inst, "find_hit", in, base + (uint32_t)strlen(q) + 1, exp, 4); }
    { int count = 2; const char *names[] = { "alpha", "beta" };
      in[0] = SPANDIFF_OP_FIND; in[1] = (uint8_t)count;
      for (int i = 0; i < count; i++) {
          memset(in + 2 + i * 64, 0, 64);
          memcpy(in + 2 + i * 64, names[i], strlen(names[i]));
      }
      uint32_t base = 2 + (uint32_t)count * 64;
      const char *q = "nope"; memcpy(in + base, q, strlen(q) + 1);
      put32(exp, (uint32_t)(int32_t)(-1));
      diff_case(utest_result, aot_env, aot_inst, "find_miss", in, base + (uint32_t)strlen(q) + 1, exp, 4); }

    /* OP_FIND exact boundary: need == in_len (the name table consumes the whole
     * input, no query bytes) must be REJECTED, not read one-past the buffer.
     * Regression for the `need >= in_len` guard in spandiff_ops.h. Returns a
     * negative SPANDIFF_ERR, so assert it directly on native + interp (+ AOT). */
    { memset(in, 0, 2 + 64);
      in[0] = SPANDIFF_OP_FIND; in[1] = 1; memcpy(in + 2, "only", 4);
      uint32_t need = 2u + 64u;   /* names table only; NO query bytes appended */
      uint8_t no[OUT_CAP];
      int nl = spandiff_run((const hull_span_u8 *)in, need, (hull_span_u8 *)no, OUT_CAP);
      ASSERT_EQ(nl, SPANDIFF_ERR);
      uint8_t go[OUT_CAP];
      int gl = run_guest(g_inst, g_env, in, need, go);
      ASSERT_EQ(gl, SPANDIFF_ERR);
      if (spandiff_aot_len > 0 && aot_inst) {
          uint8_t ao[OUT_CAP];
          int al = run_guest(aot_inst, aot_env, in, need, ao);
          ASSERT_EQ(al, SPANDIFF_ERR);
      } }

    /* OP_FIND with a query that has NO terminating NUL within the input must be
     * rejected (else hull_span_find scans past the buffer). Regression for the
     * NUL-termination check in spandiff_ops.h. */
    { memset(in, 0, 2 + 64);
      in[0] = SPANDIFF_OP_FIND; in[1] = 1; memcpy(in + 2, "only", 4);
      uint32_t base = 2u + 64u;
      in[base] = 'x'; in[base + 1] = 'y'; in[base + 2] = 'z'; in[base + 3] = 'w'; /* 4 bytes, NO NUL */
      uint32_t total = base + 4u;
      uint8_t no[OUT_CAP];
      ASSERT_EQ(spandiff_run((const hull_span_u8 *)in, total, (hull_span_u8 *)no, OUT_CAP), SPANDIFF_ERR);
      uint8_t go[OUT_CAP];
      ASSERT_EQ(run_guest(g_inst, g_env, in, total, go), SPANDIFF_ERR);
      if (spandiff_aot_len > 0 && aot_inst) {
          uint8_t ao[OUT_CAP];
          ASSERT_EQ(run_guest(aot_inst, aot_env, in, total, ao), SPANDIFF_ERR);
      } }

    /* OP_READ malformed-header boundary: the header is 19 bytes, so in_len == 18
     * must be REJECTED (an 18-byte input would over-read off/len and underflow
     * avail). in_len == 19 (header only, len == 0) is the minimal VALID input and
     * a read at any offset returns HULL_SPAN_ERR_RANGE. Regression for the
     * `in_len < 19` guard in spandiff_ops.h. */
    { memset(in, 0, 19);
      in[0] = SPANDIFF_OP_READ; in[1] = SPANDIFF_K_U8; in[2] = 0; /* off=0, len=0 */
      uint8_t no[OUT_CAP];
      /* 18 bytes → malformed → SPANDIFF_ERR on native + interp + AOT */
      ASSERT_EQ(spandiff_run((const hull_span_u8 *)in, 18, (hull_span_u8 *)no, OUT_CAP), SPANDIFF_ERR);
      uint8_t go[OUT_CAP];
      ASSERT_EQ(run_guest(g_inst, g_env, in, 18, go), SPANDIFF_ERR);
      if (spandiff_aot_len > 0 && aot_inst) {
          uint8_t ao[OUT_CAP];
          ASSERT_EQ(run_guest(aot_inst, aot_env, in, 18, ao), SPANDIFF_ERR);
      }
      /* 19 bytes (len=0) → valid input, read rejected as out-of-range */
      exp_range(exp);
      diff_case(utest_result, aot_env, aot_inst, "read_hdr19_lenzero", in, 19, exp, 4); }

    /* ══ bounded typed accessors: BE / signed / float, + reject paths ══════════
     * Window W (16 bytes) with distinctive low + high-bit bytes. */
    static const uint8_t W[16] = { 0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                                   0x80,0x90,0xA0,0xB0,0xC0,0xD0,0xE0,0xF0 };
    uint32_t n;

    /* ── big-endian accessors (independent LE-differs oracles) ── */
    n = mk_read(in, SPANDIFF_K_U16, 1, 0, 16, W, 16); exp_ok(exp, 0x0102ull);
    diff_case(utest_result, aot_env, aot_inst, "read_be16", in, n, exp, 12);
    n = mk_read(in, SPANDIFF_K_U32, 1, 0, 16, W, 16); exp_ok(exp, 0x01020304ull);
    diff_case(utest_result, aot_env, aot_inst, "read_be32", in, n, exp, 12);
    n = mk_read(in, SPANDIFF_K_U64, 1, 0, 16, W, 16); exp_ok(exp, 0x0102030405060708ull);
    diff_case(utest_result, aot_env, aot_inst, "read_be64", in, n, exp, 12);

    /* ── signed 8/16/32/64 (negative / high-bit) ── */
    n = mk_read(in, SPANDIFF_K_I8, 0, 8, 16, W, 16);  exp_ok(exp, (uint64_t)(int64_t)-128);
    diff_case(utest_result, aot_env, aot_inst, "read_i8_neg", in, n, exp, 12);
    n = mk_read(in, SPANDIFF_K_I8, 0, 0, 16, W, 16);  exp_ok(exp, 1ull);
    diff_case(utest_result, aot_env, aot_inst, "read_i8_pos", in, n, exp, 12);
    n = mk_read(in, SPANDIFF_K_I16, 0, 8, 16, W, 16); exp_ok(exp, (uint64_t)(int64_t)(int16_t)0x9080);
    diff_case(utest_result, aot_env, aot_inst, "read_i16le_neg", in, n, exp, 12);
    n = mk_read(in, SPANDIFF_K_I32, 0, 8, 16, W, 16); exp_ok(exp, (uint64_t)(int64_t)(int32_t)0xB0A09080u);
    diff_case(utest_result, aot_env, aot_inst, "read_i32le_neg", in, n, exp, 12);
    n = mk_read(in, SPANDIFF_K_I64, 0, 8, 16, W, 16); exp_ok(exp, 0xF0E0D0C0B0A09080ull);
    diff_case(utest_result, aot_env, aot_inst, "read_i64le_neg", in, n, exp, 12);
    n = mk_read(in, SPANDIFF_K_U8, 0, 8, 16, W, 16);  exp_ok(exp, 0x80ull);
    diff_case(utest_result, aot_env, aot_inst, "read_u8_high", in, n, exp, 12);

    /* ── float accessors: special-value bit patterns, incl. misaligned ── */
    { uint8_t wf[5] = { 0xFF, 0x00, 0x00, 0xC0, 0x7F };   /* NaN 0x7FC00000 LE at off 1 */
      n = mk_read(in, SPANDIFF_K_F32, 0, 1, 5, wf, 5); exp_ok(exp, 0x7FC00000ull);
      diff_case(utest_result, aot_env, aot_inst, "read_f32le_nan_off1", in, n, exp, 12); }
    { uint8_t wf[4] = { 0x00, 0x00, 0x00, 0x80 };         /* -0.0f 0x80000000 LE */
      n = mk_read(in, SPANDIFF_K_F32, 0, 0, 4, wf, 4); exp_ok(exp, 0x80000000ull);
      diff_case(utest_result, aot_env, aot_inst, "read_f32le_negzero", in, n, exp, 12); }
    { uint8_t wf[4] = { 0x3F, 0x80, 0x00, 0x00 };         /* 1.0f 0x3F800000 BE */
      n = mk_read(in, SPANDIFF_K_F32, 1, 0, 4, wf, 4); exp_ok(exp, 0x3F800000ull);
      diff_case(utest_result, aot_env, aot_inst, "read_f32be_one", in, n, exp, 12); }
    { uint8_t wf[8] = { 0,0,0,0,0,0,0xF0,0x7F };          /* +Inf f64 0x7FF0.. LE */
      n = mk_read(in, SPANDIFF_K_F64, 0, 0, 8, wf, 8); exp_ok(exp, 0x7FF0000000000000ull);
      diff_case(utest_result, aot_env, aot_inst, "read_f64le_inf", in, n, exp, 12); }
    { uint8_t wf[8] = { 0xFF,0xF0,0,0,0,0,0,0 };          /* -Inf f64 0xFFF0.. BE */
      n = mk_read(in, SPANDIFF_K_F64, 1, 0, 8, wf, 8); exp_ok(exp, 0xFFF0000000000000ull);
      diff_case(utest_result, aot_env, aot_inst, "read_f64be_neginf", in, n, exp, 12); }

    /* ── one-past + width-straddling rejection for every accessor width ── */
    exp_range(exp);
    n = mk_read(in, SPANDIFF_K_U8,  0, 16, 16, W, 16); diff_case(utest_result, aot_env, aot_inst, "read_u8_onepast",   in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_U16, 0, 15, 16, W, 16); diff_case(utest_result, aot_env, aot_inst, "read_u16_straddle", in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_U16, 0, 16, 16, W, 16); diff_case(utest_result, aot_env, aot_inst, "read_u16_onepast",  in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_U32, 0, 13, 16, W, 16); diff_case(utest_result, aot_env, aot_inst, "read_u32_straddle", in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_U32, 0, 16, 16, W, 16); diff_case(utest_result, aot_env, aot_inst, "read_u32_onepast",  in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_U64, 0,  9, 16, W, 16); diff_case(utest_result, aot_env, aot_inst, "read_u64_straddle", in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_U64, 0, 16, 16, W, 16); diff_case(utest_result, aot_env, aot_inst, "read_u64_onepast",  in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_F32, 1, 13, 16, W, 16); diff_case(utest_result, aot_env, aot_inst, "read_f32_straddle", in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_F64, 0,  9, 16, W, 16); diff_case(utest_result, aot_env, aot_inst, "read_f64_straddle", in, n, exp, 4);

    /* ── overflow offsets near UINT64_MAX (off+width would wrap): must reject ── */
    n = mk_read(in, SPANDIFF_K_U16, 0, 0xFFFFFFFFFFFFFFFFull, 16, W, 16);
    diff_case(utest_result, aot_env, aot_inst, "read_u16_off_uint64max", in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_U32, 0, 0xFFFFFFFFFFFFFFFEull, 16, W, 16);
    diff_case(utest_result, aot_env, aot_inst, "read_u32_off_near_max", in, n, exp, 4);
    n = mk_read(in, SPANDIFF_K_U64, 0, 0xFFFFFFFFFFFFFFF8ull, 16, W, 16);
    diff_case(utest_result, aot_env, aot_inst, "read_u64_off_near_max", in, n, exp, 4);

    /* Report AOT coverage so CI can assert the AOT diff was actually exercised
     * (mirrors the guarded-subrange SKIPPED convention). */
    if (spandiff_aot_len > 0)
        fprintf(stderr, "spandiff.aot_diff ran (%u bytes)\n", spandiff_aot_len);
    else
        fprintf(stderr, "SKIPPED spandiff.aot_diff (wamrc unavailable)\n");

    /* cleanup */
    if (aot_inst) {
        wasm_runtime_destroy_exec_env(aot_env);
        wasm_runtime_deinstantiate(aot_inst);
        wasm_runtime_unload(aot_mod);
        free(aot_buf);
    }
    wasm_runtime_destroy_exec_env(g_env);
    wasm_runtime_deinstantiate(g_inst);
    wasm_runtime_unload(g_mod);
    free(g_buf);
    wasm_runtime_destroy();
}

UTEST_MAIN();
