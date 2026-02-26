/*
 * hull_cap_body.c — Body reader factory and extraction for Hull runtimes
 *
 * Wraps Keel's kl_body_reader_buffer with a 1 MB limit.
 * Both JS and Lua bindings use hl_cap_body_data() to extract
 * the buffered body after on_complete fires.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/body.h"
#include "hull/limits.h"
#include <stddef.h>

KlBodyReader *hl_cap_body_factory(KlAllocator *alloc, const KlRequest *req,
                                  void *user_data)
{
    (void)user_data;
    return kl_body_reader_buffer(alloc, req,
                                 (void *)(size_t)HL_BODY_MAX_SIZE);
}

size_t hl_cap_body_data(const KlBodyReader *reader, const char **out_data)
{
    if (!reader) {
        *out_data = NULL;
        return 0;
    }
    const KlBufReader *br = (const KlBufReader *)reader;
    *out_data = br->data;
    return br->len;
}
