/*
 * cap/http_parser.h — Pluggable HTTP response parser vtable
 *
 * Mirrors Keel's KlParser pattern for request parsing, applied to
 * HTTP response parsing. Allows swapping the parser backend (llhttp,
 * picohttpparser, etc.) independently of the HTTP client logic.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_HTTP_PARSER_H
#define HL_CAP_HTTP_PARSER_H

#include <stddef.h>

#include <keel/allocator.h>

/* Forward declaration — full struct defined in http.h */
struct HlHttpResponse;

/**
 * @brief Result codes for response parsing.
 */
typedef enum {
    HL_HTTP_PARSE_OK,         /**< Complete response parsed */
    HL_HTTP_PARSE_INCOMPLETE, /**< Need more data */
    HL_HTTP_PARSE_ERROR       /**< Parse error */
} HlHttpParseResult;

/**
 * @brief Pluggable HTTP response parser vtable.
 *
 * One instance per request. The parser is fed recv'd bytes in a loop
 * until it returns OK (complete) or ERROR.
 */
typedef struct HlHttpParser HlHttpParser;

struct HlHttpParser {
    /**
     * @brief Parse a chunk of response data.
     *
     * @param self     Parser instance.
     * @param resp     Response struct to populate.
     * @param buf      Input data buffer.
     * @param len      Length of input data.
     * @param consumed Output: bytes consumed from buf.
     * @return Parse result.
     */
    HlHttpParseResult (*parse)(HlHttpParser *self,
                                struct HlHttpResponse *resp,
                                const char *buf, size_t len,
                                size_t *consumed);

    /**
     * @brief Reset parser for reuse (not typically needed for HTTP client).
     */
    void (*reset)(HlHttpParser *self);

    /**
     * @brief Free all parser resources.
     */
    void (*destroy)(HlHttpParser *self);
};

/**
 * @brief Create an llhttp-based response parser.
 *
 * @param max_response_size  Maximum allowed response body size (0 = unlimited).
 * @return New parser instance, or NULL on failure.
 */
HlHttpParser *hl_http_parser_llhttp(size_t max_response_size,
                                     KlAllocator *alloc);

#endif /* HL_CAP_HTTP_PARSER_H */
