/*
 * fuzz_span_sdk.c: libFuzzer harness for the guest-side mapped-span SDK math
 * (templates/hull_span.h) - the ATTACKER-CONTROLLED surface a WASM plugin runs on
 * the span metadata + window contents. Every input here (record bytes, rec_len,
 * read offsets, scratch pointer) is treated as adversarial. Exercises, under
 * ASan+UBSan:
 *
 *   - hull_span_decode(): parse an arbitrary HlSpanMetaV1 record at an arbitrary
 *     rec_len (the version/struct_size/short-record gates + the fixed-width field
 *     reads + the always-terminated name copy).
 *   - the typed accessors hull_span_read_*(): a TRUTHFUL window (w = the fuzz
 *     buffer, len = its real size) read at adversarial offsets (0, len-1, len,
 *     len+1, near-UINT64_MAX, > 4 GiB). If hull_span__fits() ever lets an OOB /
 *     width-straddling / overflowing offset through, ASan traps on the read of
 *     the poisoned redzone past the libFuzzer input. This is the core "span
 *     offset/length calculation" fuzz the spec asks for.
 *   - hull_span__narrow(): an arbitrary scratch pointer narrowed to the
 *     (i32,i32,i32) host_call ABI (the >= 4 GiB reject that prevents a silent
 *     truncation handing the host a bogus in-range offset).
 *   - hull_span_find(): a name lookup over the decoded span.
 *
 * The header is freestanding (no Hull link deps). hull_span_setup() references
 * host_call only through HULL_SPAN_HOST_CALL, which we define to a no-op so no
 * symbol is needed (setup itself is not driven here).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HULL_SPAN_HOST_CALL(op, a, b) (0)   /* setup's query is not exercised here */
#include "hull_span.h"

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* (1) decode: [u32 rec_len][record bytes...]. rec_len is fuzzer-chosen but
     * clamped to the bytes we actually hold - decode trusts rec_len as the count
     * of valid bytes by contract, so feeding a larger value would test the caller
     * contract, not the decoder. */
    HullSpan sp;
    int have_span = 0;
    if (size >= 4) {
        uint32_t rec_len = (uint32_t)(data[0] | (data[1] << 8) | (data[2] << 16)
                                      | ((uint32_t)data[3] << 24));
        size_t avail = size - 4;
        if (rec_len > avail) rec_len = (uint32_t)avail;
        if (hull_span_decode(data + 4, rec_len, &sp) == 0) {
            have_span = 1;
            (void)hull_span_find(&sp, 1, sp.name);   /* self-name must resolve to 0 */
            (void)hull_span_find(&sp, 1, "nope");
        }
    }

    /* (2) accessors: truthful window + adversarial offsets. Any escape is an ASan
     * OOB on the read past the (poisoned) libFuzzer input buffer. */
    {
        const void *w = data;
        uint64_t n = (uint64_t)size;
        uint64_t offs[] = {
            0, 1, n ? n - 1 : 0, n, n + 1, n + 3, n + 7,
            size >= 8 ? rd_u64(data) : 0,
            UINT64_MAX, UINT64_MAX - 3, UINT64_MAX - 7,
            n + 0x100000000ull,                     /* > 4 GiB past a small window */
        };
        for (size_t k = 0; k < sizeof(offs) / sizeof(offs[0]); k++) {
            uint64_t o = offs[k];
            uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64v;
            int8_t i8; int16_t i16; int32_t i32; int64_t i64;
            float f; double d;
            (void)hull_span_read_u8(w, n, o, &u8);   (void)hull_span_read_i8(w, n, o, &i8);
            (void)hull_span_read_u16le(w, n, o, &u16); (void)hull_span_read_u16be(w, n, o, &u16);
            (void)hull_span_read_i16le(w, n, o, &i16); (void)hull_span_read_i16be(w, n, o, &i16);
            (void)hull_span_read_u32le(w, n, o, &u32); (void)hull_span_read_u32be(w, n, o, &u32);
            (void)hull_span_read_i32le(w, n, o, &i32); (void)hull_span_read_i32be(w, n, o, &i32);
            (void)hull_span_read_u64le(w, n, o, &u64v); (void)hull_span_read_u64be(w, n, o, &u64v);
            (void)hull_span_read_i64le(w, n, o, &i64); (void)hull_span_read_i64be(w, n, o, &i64);
            (void)hull_span_read_f32le(w, n, o, &f);  (void)hull_span_read_f32be(w, n, o, &f);
            (void)hull_span_read_f64le(w, n, o, &d);  (void)hull_span_read_f64be(w, n, o, &d);
        }
    }

    /* (3) narrow: arbitrary scratch pointer -> i32 ABI (>= 4 GiB must reject).
     * UBSan covers the cast/shift; we only need it not to crash or UB. */
    if (size >= 8) {
        hull_span_i32 out;
        (void)hull_span__narrow((hull_span_uptr)rd_u64(data), &out);
    }

    (void)have_span;
    return 0;
}
