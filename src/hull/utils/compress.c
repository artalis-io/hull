/*
 * compress.c — Response compression helper
 *
 * Checks Accept-Encoding and uses Keel's compression vtable to
 * compress response bodies when beneficial.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/utils/compress.h"

#include <string.h>

/* Check if "gzip" appears in Accept-Encoding header value */
static int accepts_gzip(KlRequest *req)
{
    const char *ae = kl_request_header(req, "Accept-Encoding");
    if (!ae)
        return 0;
    return strstr(ae, "gzip") != NULL;
}

void hl_maybe_compress(KlRequest *req, KlResponse *res,
                       KlCompressConfig *cfg,
                       const char *data, size_t len)
{
    if (cfg && len >= HL_COMPRESS_MIN_SIZE && accepts_gzip(req)) {
        if (kl_response_body_compress(res, cfg, data, len) == 0)
            return; /* success — body + Content-Encoding set */
    }

    /* Fallback: uncompressed */
    kl_response_body_copy(res, data, len);
}
