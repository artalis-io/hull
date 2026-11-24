/*
 * libFuzzer harness for the exec slot matcher (hl_exec_slot_match through
 * hl_exec_check_call) and the builder's `chars` parser (cap/exec.c).
 *
 * Input: byte 0 = flags (allow_dash, repeat, use chars), bytes 1..2 = min /
 * max, then NUL-separated fields: prefix, suffix, chars spec, then the call's
 * argument values. Every value an app passes to exec.run reaches the matcher,
 * so it must decide on any bytes without reading past them; a slot that
 * builds and validates must never admit a value starting with '-' unless it
 * allows a dash, nor one holding a NUL.
 *
 * Run:
 *   ./fuzz/fuzz_exec_slot fuzz/corpus_exec_slot/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/exec.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS (3 + HL_EXEC_MAX_REPEAT)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 3) return 0;
    uint8_t flags = data[0];
    int64_t min = data[1] ? data[1] : -1;
    int64_t max = data[2] ? (int64_t)data[2] * 16 : -1;

    /* Each field its own exact-sized heap copy, so ASan brackets it. */
    char *fields[MAX_FIELDS];
    size_t flen[MAX_FIELDS];
    size_t nf = 0;
    size_t i = 3;
    while (i <= size && nf < MAX_FIELDS) {
        size_t j = i;
        while (j < size && data[j] != 0) j++;
        flen[nf] = j - i;
        fields[nf] = malloc(flen[nf] ? flen[nf] : 1);
        if (!fields[nf]) break;
        if (flen[nf]) memcpy(fields[nf], data + i, flen[nf]);
        nf++;
        i = j + 1;
    }
    if (nf < 3) goto done;

    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    hl_exec_b_cmd(&b, "c", 1, "/usr/bin/t", 10, -1, -1, -1, -1, 0);
    for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    size_t m = hl_exec_b_count_begin(&b);
    int dash = flags & 1, repeat = (flags >> 1) & 1, chars = (flags >> 2) & 1;
    hl_exec_b_slot(&b, dash, min, max, repeat, repeat ? (flags >> 4) & 3 : -1,
                   repeat ? HL_EXEC_MAX_REPEAT : -1,
                   fields[0], flen[0], fields[1], flen[1],
                   chars ? fields[2] : NULL, chars ? flen[2] : 0);
    hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    hl_exec_b_count_end(&b, m, 1);
    if (hl_exec_b_finish(&b) == 0) {
        const char *args[MAX_FIELDS];
        for (size_t k = 3; k < nf; k++) args[k - 3] = fields[k];
        char err[256];
        int rc = hl_exec_check_call(b.data, b.len, "c", 1, args, flen + 3, nf - 3,
                                    NULL, err, sizeof err);
        if (rc == HL_EXEC_OK) {
            for (size_t k = 3; k < nf; k++) {
                if (flen[k] == 0) abort();                         /* never empty */
                if (memchr(fields[k], 0, flen[k])) abort();        /* never a NUL */
                if (fields[k][0] == '-' && !dash) abort();         /* never an option */
            }
        }
        hl_exec_buf_free(&b);
    }
done:
    for (size_t k = 0; k < nf; k++) free(fields[k]);
    return 0;
}
