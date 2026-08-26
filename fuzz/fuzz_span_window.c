/*
 * fuzz_span_window.c: libFuzzer harness for the mapped-WINDOW geometry math
 * (hl_cap_fs_mmap_window_geometry) - the host-side page-alignment + EOF-clamp +
 * overflow-safe page-rounding of an (offset, length) window request over a file.
 * This is the "mapping/window operations" arithmetic the spec asks to fuzz: the
 * function turns four untrusted-in-the-limit u64s into an mmap offset/length pair,
 * and a wrong bound here would mmap the wrong bytes or overflow.
 *
 * Fuzzes the four u64 inputs and, on the success path, asserts the documented
 * invariants; UBSan catches any signed/unsigned overflow or UB in the math, and
 * the asserts catch a logically-wrong (but not UB) result. cap/fs.c is compiled
 * with -ffunction-sections + dead-strip in the fuzz rule, so only this pure leaf
 * (which calls nothing external) is linked - no alloc/audit/mmap deps.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/cap/fs.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 32) return 0;
    uint64_t v[4];
    memcpy(v, data, 32);
    uint64_t offset = v[0], length = v[1], file_size = v[2], page_size = v[3];

    uint64_t map_off = 0, map_len = 0, slop = 0, eff_len = 0;
    const char *err = NULL;
    int rc = hl_cap_fs_mmap_window_geometry(offset, length, file_size, page_size,
                                            &map_off, &map_len, &slop, &eff_len, &err);
    if (rc == 0) {
        /* Documented success invariants (see include/hull/cap/fs.h). */
        assert(page_size != 0);
        assert(length != 0);
        assert(offset < file_size);          /* offset strictly inside the file */
        assert(map_off <= offset);           /* mapping starts at/behind the window */
        assert(slop == offset - map_off);    /* window begins `slop` into the mapping */
        assert(slop < page_size);            /* slop is a within-page remainder */
        assert(eff_len >= 1);                /* EOF clamp never yields an empty window */
        assert(eff_len <= length);           /* clamp only shrinks */
        assert(eff_len <= file_size - offset); /* window stays within the file (no wrap) */
        assert(map_len % page_size == 0);    /* mapping is a whole number of pages */
        assert(map_len >= slop + eff_len);   /* mapping fully covers the window */
    } else {
        assert(err != NULL);                 /* a failure always names a reason */
    }
    return 0;
}
