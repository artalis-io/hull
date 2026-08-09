/*
 * fuzz_respwire.c: RESP2/3 reply parser over untrusted server bytes.
 *
 * Drives hl_resp_parse across arbitrary input: it must only decode or report
 * NEED_MORE / PARSE_ERR, never read out of bounds, recurse without bound, or
 * loop forever. ASan/UBSan catch the first; the progress + iteration guards
 * catch the last.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/respwire.h"
#include <stddef.h>
#include <stdint.h>

/* Bump arena for aggregate items; reset per parse. */
static uint8_t g_arena[1u << 20];
static size_t  g_arena_off;
static void *fz_alloc(void *ctx, size_t n) {
    (void)ctx;
    if (n > sizeof g_arena - g_arena_off) return NULL;
    void *p = g_arena + g_arena_off;
    g_arena_off += n;
    return p;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t off = 0;
    int iters = 0;
    while (off < size && iters++ < 8192) {
        g_arena_off = 0;
        size_t consumed = 0;
        HlRespValue v;
        HlRespResult r = hl_resp_parse(data + off, size - off, &consumed, &v, fz_alloc, NULL);
        if (r != HL_RESP_OK) break;
        if (consumed == 0) break;       /* no forward progress -> stop */
        off += consumed;
    }
    return 0;
}
