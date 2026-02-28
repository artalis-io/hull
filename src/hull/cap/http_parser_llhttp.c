/*
 * http_parser_llhttp.c — llhttp backend for HlHttpParser vtable
 *
 * Uses llhttp in HTTP_RESPONSE mode to parse response status, headers,
 * and body. Accumulates headers and body into the HlHttpResponse struct.
 *
 * All dynamic memory goes through the KlAllocator vtable for consistency
 * with Keel's allocator discipline.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/http_parser.h"
#include "hull/cap/http.h"

#include <keel/allocator.h>
#include <llhttp.h>

#include <stdint.h>
#include <string.h>

#define MAX_RESPONSE_HEADERS 64

/* ── Parser state ────────────────────────────────────────────────── */

typedef struct {
    HlHttpParser     base;         /* vtable — must be first */
    llhttp_t         parser;
    llhttp_settings_t settings;
    KlAllocator     *alloc;        /* allocator for all dynamic memory */

    /* Accumulation state */
    HlHttpResponse  *resp;         /* current response (set per parse call) */
    size_t           max_body;     /* max response body size */
    int              complete;     /* 1 = on_message_complete fired */
    int              error;        /* 1 = error occurred */

    /* Header accumulation */
    char            *hdr_name;
    size_t           hdr_name_len;
    size_t           hdr_name_cap;
    char            *hdr_value;
    size_t           hdr_value_len;
    size_t           hdr_value_cap;

    /* Body accumulation */
    char            *body;
    size_t           body_len;
    size_t           body_cap;

    /* Header array */
    HlHttpHeader    *headers;
    int              num_headers;
    int              headers_cap;
} LlhttpParser;

/* ── String accumulation helpers ─────────────────────────────────── */

static int accum_append(KlAllocator *alloc,
                         char **buf, size_t *len, size_t *cap,
                         const char *data, size_t data_len)
{
    /* Check for overflow before addition */
    if (data_len > SIZE_MAX - *len)
        return -1;
    size_t needed = *len + data_len;
    if (needed > *cap) {
        size_t new_cap = *cap ? *cap * 2 : 256;
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2)
                return -1;
            new_cap *= 2;
        }
        char *new_buf = kl_realloc(alloc, *buf, *cap, new_cap);
        if (!new_buf)
            return -1;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    return 0;
}

/* ── Flush accumulated header ────────────────────────────────────── */

static int flush_header(LlhttpParser *p)
{
    if (p->hdr_name_len == 0)
        return 0;

    /* Grow headers array if needed */
    if (p->num_headers >= p->headers_cap) {
        if (p->num_headers >= MAX_RESPONSE_HEADERS)
            return -1;
        int new_cap = p->headers_cap ? p->headers_cap * 2 : 16;
        if (new_cap > MAX_RESPONSE_HEADERS)
            new_cap = MAX_RESPONSE_HEADERS;
        size_t old_size = (size_t)p->headers_cap * sizeof(HlHttpHeader);
        size_t new_size = (size_t)new_cap * sizeof(HlHttpHeader);
        HlHttpHeader *new_hdrs = kl_realloc(p->alloc, p->headers,
                                              old_size, new_size);
        if (!new_hdrs)
            return -1;
        p->headers = new_hdrs;
        p->headers_cap = new_cap;
    }

    /* NUL-terminate name and value in accum buffers */
    if (accum_append(p->alloc, &p->hdr_name, &p->hdr_name_len,
                      &p->hdr_name_cap, "\0", 1) != 0)
        return -1;
    if (accum_append(p->alloc, &p->hdr_value, &p->hdr_value_len,
                      &p->hdr_value_cap, "\0", 1) != 0)
        return -1;

    /* Make exact-sized copies so kl_free gets the right size (strlen+1) */
    char *name_copy = kl_malloc(p->alloc, p->hdr_name_len);
    if (!name_copy)
        return -1;
    memcpy(name_copy, p->hdr_name, p->hdr_name_len);

    char *value_copy = kl_malloc(p->alloc, p->hdr_value_len);
    if (!value_copy) {
        kl_free(p->alloc, name_copy, p->hdr_name_len);
        return -1;
    }
    memcpy(value_copy, p->hdr_value, p->hdr_value_len);

    /* Free accumulation buffers */
    kl_free(p->alloc, p->hdr_name, p->hdr_name_cap);
    kl_free(p->alloc, p->hdr_value, p->hdr_value_cap);

    /* Store exact copies in headers array */
    p->headers[p->num_headers].name = name_copy;
    p->headers[p->num_headers].value = value_copy;
    p->num_headers++;

    /* Reset accumulators */
    p->hdr_name = NULL;
    p->hdr_name_len = 0;
    p->hdr_name_cap = 0;
    p->hdr_value = NULL;
    p->hdr_value_len = 0;
    p->hdr_value_cap = 0;

    return 0;
}

/* ── llhttp callbacks ────────────────────────────────────────────── */

static int on_status(llhttp_t *parser, const char *at, size_t len)
{
    (void)at; (void)len;
    LlhttpParser *p = (LlhttpParser *)parser->data;
    p->resp->status = (int)parser->status_code;
    return 0;
}

static int on_header_field(llhttp_t *parser, const char *at, size_t len)
{
    LlhttpParser *p = (LlhttpParser *)parser->data;

    /* If we have a pending value, flush the previous header */
    if (p->hdr_value_len > 0) {
        if (flush_header(p) != 0)
            return -1;
    }

    return accum_append(p->alloc, &p->hdr_name, &p->hdr_name_len,
                         &p->hdr_name_cap, at, len);
}

static int on_header_value(llhttp_t *parser, const char *at, size_t len)
{
    LlhttpParser *p = (LlhttpParser *)parser->data;
    return accum_append(p->alloc, &p->hdr_value, &p->hdr_value_len,
                         &p->hdr_value_cap, at, len);
}

static int on_headers_complete(llhttp_t *parser)
{
    LlhttpParser *p = (LlhttpParser *)parser->data;

    /* Flush last header */
    if (p->hdr_name_len > 0) {
        if (flush_header(p) != 0)
            return -1;
    }

    /* Capture status (in case on_status wasn't called for some reason) */
    p->resp->status = (int)parser->status_code;

    return 0;
}

static int on_body(llhttp_t *parser, const char *at, size_t len)
{
    LlhttpParser *p = (LlhttpParser *)parser->data;

    /* Check body size limit */
    if (p->max_body > 0 && p->body_len + len > p->max_body) {
        p->error = 1;
        return -1;
    }

    return accum_append(p->alloc, &p->body, &p->body_len,
                         &p->body_cap, at, len);
}

static int on_message_complete(llhttp_t *parser)
{
    LlhttpParser *p = (LlhttpParser *)parser->data;
    p->complete = 1;
    return HPE_PAUSED;  /* pause so we stop consuming data */
}

/* ── Transfer parser data to response ─────────────────────────────── */

static HlHttpParseResult transfer_to_response(LlhttpParser *p,
                                                HlHttpResponse *resp)
{
    /* NUL-terminate body and realloc to exact size */
    if (p->body && p->body_len > 0) {
        char *exact = kl_realloc(p->alloc, p->body, p->body_cap,
                                  p->body_len + 1);
        if (!exact)
            return HL_HTTP_PARSE_ERROR;
        exact[p->body_len] = '\0';
        p->body = exact;
        p->body_cap = p->body_len + 1;
    }

    /* Make exact-sized copy of headers array */
    if (p->headers && p->num_headers > 0) {
        size_t exact_size = (size_t)p->num_headers * sizeof(HlHttpHeader);
        size_t old_size = (size_t)p->headers_cap * sizeof(HlHttpHeader);
        HlHttpHeader *exact_hdrs = kl_malloc(p->alloc, exact_size);
        if (!exact_hdrs)
            return HL_HTTP_PARSE_ERROR;
        memcpy(exact_hdrs, p->headers, exact_size);
        kl_free(p->alloc, p->headers, old_size);
        p->headers = exact_hdrs;
    }

    /* Transfer ownership to response */
    resp->body = p->body;
    resp->body_len = p->body_len;
    resp->headers = p->headers;
    resp->num_headers = p->num_headers;

    /* Clear parser pointers */
    p->body = NULL;
    p->body_len = 0;
    p->body_cap = 0;
    p->headers = NULL;
    p->num_headers = 0;
    p->headers_cap = 0;

    return HL_HTTP_PARSE_OK;
}

/* ── HlHttpParser vtable ────────────────────────────────────────── */

static HlHttpParseResult parser_parse(HlHttpParser *self,
                                       struct HlHttpResponse *resp,
                                       const char *buf, size_t len,
                                       size_t *consumed)
{
    LlhttpParser *p = (LlhttpParser *)self;
    p->resp = resp;

    enum llhttp_errno err = llhttp_execute(&p->parser, buf, len);
    *consumed = (size_t)(llhttp_get_error_pos(&p->parser) - buf);

    if (p->error)
        return HL_HTTP_PARSE_ERROR;

    if (p->complete)
        return transfer_to_response(p, resp);

    if (err == HPE_PAUSED) {
        llhttp_resume(&p->parser);
        return transfer_to_response(p, resp);
    }

    if (err != HPE_OK)
        return HL_HTTP_PARSE_ERROR;

    return HL_HTTP_PARSE_INCOMPLETE;
}

static void parser_reset(HlHttpParser *self)
{
    LlhttpParser *p = (LlhttpParser *)self;
    llhttp_reset(&p->parser);
    p->complete = 0;
    p->error = 0;

    kl_free(p->alloc, p->hdr_name, p->hdr_name_cap);
    kl_free(p->alloc, p->hdr_value, p->hdr_value_cap);
    kl_free(p->alloc, p->body, p->body_cap);

    p->hdr_name = NULL;
    p->hdr_name_len = 0;
    p->hdr_name_cap = 0;
    p->hdr_value = NULL;
    p->hdr_value_len = 0;
    p->hdr_value_cap = 0;
    p->body = NULL;
    p->body_len = 0;
    p->body_cap = 0;

    for (int i = 0; i < p->num_headers; i++) {
        kl_free(p->alloc, (char *)p->headers[i].name,
                strlen(p->headers[i].name) + 1);
        kl_free(p->alloc, (char *)p->headers[i].value,
                strlen(p->headers[i].value) + 1);
    }
    kl_free(p->alloc, p->headers,
            (size_t)p->headers_cap * sizeof(HlHttpHeader));
    p->headers = NULL;
    p->num_headers = 0;
    p->headers_cap = 0;
}

static void parser_destroy(HlHttpParser *self)
{
    if (!self)
        return;

    LlhttpParser *p = (LlhttpParser *)self;
    KlAllocator *alloc = p->alloc;

    kl_free(alloc, p->hdr_name, p->hdr_name_cap);
    kl_free(alloc, p->hdr_value, p->hdr_value_cap);
    kl_free(alloc, p->body, p->body_cap);

    for (int i = 0; i < p->num_headers; i++) {
        kl_free(alloc, (char *)p->headers[i].name,
                strlen(p->headers[i].name) + 1);
        kl_free(alloc, (char *)p->headers[i].value,
                strlen(p->headers[i].value) + 1);
    }
    kl_free(alloc, p->headers,
            (size_t)p->headers_cap * sizeof(HlHttpHeader));

    kl_free(alloc, p, sizeof(LlhttpParser));
}

/* ── Factory ─────────────────────────────────────────────────────── */

HlHttpParser *hl_http_parser_llhttp(size_t max_response_size,
                                     KlAllocator *alloc)
{
    LlhttpParser *p = kl_malloc(alloc, sizeof(*p));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));

    p->alloc = alloc;

    /* Set up vtable */
    p->base.parse   = parser_parse;
    p->base.reset   = parser_reset;
    p->base.destroy = parser_destroy;

    /* Configure llhttp callbacks */
    llhttp_settings_init(&p->settings);
    p->settings.on_status           = on_status;
    p->settings.on_header_field     = on_header_field;
    p->settings.on_header_value     = on_header_value;
    p->settings.on_headers_complete = on_headers_complete;
    p->settings.on_body             = on_body;
    p->settings.on_message_complete = on_message_complete;

    /* Initialize parser in HTTP_RESPONSE mode */
    llhttp_init(&p->parser, HTTP_RESPONSE, &p->settings);
    p->parser.data = p;

    p->max_body = max_response_size;

    return &p->base;
}
