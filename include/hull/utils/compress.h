/*
 * compress.h - Response compression helper
 *
 * Checks Accept-Encoding and compresses when beneficial.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMPRESS_H
#define HL_COMPRESS_H

#include <keel/compress.h>
#include <keel/http_compress.h>   /* kl_http_response_body_compress, KlCompressConfig */
#include <keel/http_request.h>
#include <keel/http_response.h>

#include <stddef.h>

#include "limits.h" /* HL_COMPRESS_MIN_SIZE */

/*
 * Compress response body if the client accepts gzip and the body
 * exceeds the minimum size threshold.
 *
 * Falls back to kl_http_response_body_copy() if compression is not
 * applicable (no Accept-Encoding: gzip, body too small, no config).
 *
 * @param req   Current request (checked for Accept-Encoding).
 * @param res   Response to set body on.
 * @param cfg   Compression config (NULL = no compression).
 * @param data  Body data.
 * @param len   Body data length.
 */
void hl_maybe_compress(KlHttpRequest *req, KlHttpResponse *res,
                       KlCompressConfig *cfg,
                       const char *data, size_t len);

#endif /* HL_COMPRESS_H */
