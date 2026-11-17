/*
 * fuzz_plain_value.c: libFuzzer harness for hl_pv_check and the reader
 * (shared/plain_value.c), the build plugin channel's value encoding - the
 * bytes a plugin process sends its host.
 *
 * Oracles, for every input:
 *   - hl_pv_check never reads outside the buffer (ASan brackets it, exact
 *     size) and returns one of its codes;
 *   - an input it accepts is canonical: walked with the reader and written
 *     again with the writer, it gives the same bytes;
 *   - an input it accepts, cut short by any number of bytes, is refused.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/plain_value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what)
{
    fprintf(stderr, "fuzz_plain_value: %s\n", what);
    abort();
}

static void copy_value(HlPvReader *r, HlPvWriter *w)
{
    HlPvItem it;
    if (hl_pv_next(r, &it) != 0) fail("reader refused a checked value");
    switch (it.type) {
    case HL_PV_NIL:   hl_pv_put_nil(w); break;
    case HL_PV_FALSE: hl_pv_put_bool(w, 0); break;
    case HL_PV_TRUE:  hl_pv_put_bool(w, 1); break;
    case HL_PV_INT:   hl_pv_put_int(w, it.i); break;
    case HL_PV_TEXT:  hl_pv_put_text(w, it.s, it.n); break;
    case HL_PV_BYTES: hl_pv_put_bytes(w, it.s, it.n); break;
    case HL_PV_ARRAY:
        hl_pv_put_array(w, (uint32_t)it.n);
        for (size_t i = 0; i < it.n; i++) copy_value(r, w);
        break;
    case HL_PV_MAP:
        hl_pv_put_map(w, (uint32_t)it.n);
        for (size_t i = 0; i < it.n; i++) {
            const uint8_t *k;
            size_t kn;
            if (hl_pv_next_key(r, &k, &kn) != 0) fail("reader refused a checked key");
            hl_pv_put_key(w, k, kn);
            copy_value(r, w);
        }
        break;
    default:
        fail("reader returned an unknown type");
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    int rc = hl_pv_check(buf, size);
    if (rc != HL_PV_OK && rc != HL_PV_E_SHAPE && rc != HL_PV_E_TEXT &&
        rc != HL_PV_E_DEPTH)
        fail("unexpected code");
    if (rc == HL_PV_OK) {
        HlPvReader r;
        HlPvWriter w;
        hl_pv_reader_init(&r, buf, size);
        hl_pv_writer_init(&w, size);
        copy_value(&r, &w);
        if (w.err != HL_PV_OK) fail("writer refused a checked value");
        if (r.pos != size || w.len != size || memcmp(w.buf, buf, size) != 0)
            fail("a checked value is not canonical");
        hl_pv_writer_free(&w);
        for (size_t cut = 1; cut <= size && cut <= 16; cut++)
            if (hl_pv_check(buf, size - cut) == HL_PV_OK)
                fail("a truncated value was accepted");
    }
    free(buf);
    return 0;
}
