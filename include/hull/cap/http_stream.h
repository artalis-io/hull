/*
 * cap/http_stream.h - a streamed HTTP response (hull/http-client async.stream)
 *
 * `http.fetch` hands the app the whole body once the request is done. A stream
 * hands it the status and headers as soon as they arrive, then the body chunk
 * by chunk while the request is still running: a Server-Sent Events feed, a
 * long download, a model's token stream.
 *
 * ## Shape
 *
 * Keel's async client pushes the response (on_headers, on_body); the app pulls
 * it (read). A bounded buffer sits between the two, and the read that finds it
 * empty parks the caller on the stream's op, which the next arrival completes
 * - the read model of cap/net_stream.h. Status, headers and reads all return
 * HL_HTTP_STREAM_E_AGAIN when they would wait; the binding then parks on
 * hl_http_stream_pending_op() and calls again once resumed. The op is only
 * completed when the next call will NOT return AGAIN (data, the end, an
 * error, a timeout, a close), so a resumed caller never parks twice in a row.
 *
 * ## Backpressure
 *
 * Keel's client cannot pause its reads, so the server cannot be slowed down:
 * unread bytes are held up to `buf_cap` (default 1 MiB), and a stream whose
 * reader falls that far behind ends with HL_HTTP_STREAM_E_OVERFLOW after the
 * buffered bytes are read. Real backpressure (stop reading the socket while
 * the buffer is full) needs a Keel pause / resume, a follow-up.
 *
 * ## Time
 *
 * `timeout_ms` bounds the request up to the response headers (connect, TLS,
 * send, the server's think time), resolved and clamped like http.fetch's.
 * After the headers, `idle_ms` bounds the gap between two arrivals (default
 * 60 s). `life_ms` bounds the whole stream, from the start (default
 * HL_HTTP_STREAM_LIFE_DEFAULT_MS, 10 min, clamped to
 * HL_HTTP_STREAM_MAX_LIFE_MS, 24 h, which is also Keel's whole-request
 * deadline): past it the stream ends with HL_HTTP_STREAM_E_LIFE after the
 * buffered bytes are read. Keel does not notice a client that hung up while
 * its handler waits on a stream, so a relay holds its in-flight slot until
 * the stream ends; the life is what bounds it (audit 52). A stream does not
 * follow redirects: a 3xx is the response.
 *
 * ## Lifetime
 *
 * The stream is a heap block behind an opaque handle. hl_http_stream_close
 * stops the request and its timers (the loop must still be alive);
 * hl_http_stream_free closes and frees, and is safe after the loop is gone
 * once close ran (teardown: the binding's HlAsyncTeardown entry calls close
 * while the loop is alive, the finalizer frees later). Single-threaded: the
 * event-loop thread only.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_CAP_HTTP_STREAM_H
#define HL_CAP_HTTP_STREAM_H

#include "hull/cap/http.h"

#include <stddef.h>
#include <stdint.h>

struct HlAsyncOp;
struct HlAsyncBackend;
struct HlAsyncBackendCtx;
typedef struct KlHttpServer KlHttpServer;

typedef struct HlHttpStream HlHttpStream;

#define HL_HTTP_STREAM_OK           0
#define HL_HTTP_STREAM_E_AGAIN    (-1)   /* would wait: park and call again    */
#define HL_HTTP_STREAM_E_TIMEOUT  (-2)   /* no response headers within timeout */
#define HL_HTTP_STREAM_E_IDLE     (-3)   /* nothing arrived for idle_ms        */
#define HL_HTTP_STREAM_E_OVERFLOW (-4)   /* the reader fell buf_cap behind     */
#define HL_HTTP_STREAM_E_TOO_LARGE (-5)  /* the body passed max_bytes          */
#define HL_HTTP_STREAM_E_IO       (-6)   /* connect / TLS / transport / parse  */
#define HL_HTTP_STREAM_E_CLOSED   (-7)   /* closed by the caller               */
#define HL_HTTP_STREAM_E_LIFE     (-8)   /* the stream outlived life_ms        */

#define HL_HTTP_STREAM_BUF_DEFAULT   (1024u * 1024u)
#define HL_HTTP_STREAM_BUF_MIN       (16u * 1024u)
#define HL_HTTP_STREAM_BUF_MAX       (16u * 1024u * 1024u)
#define HL_HTTP_STREAM_IDLE_DEFAULT_MS  60000
#define HL_HTTP_STREAM_IDLE_MAX_MS      (60 * 60 * 1000)        /* 1 h  */
#define HL_HTTP_STREAM_LIFE_DEFAULT_MS  (10 * 60 * 1000)        /* 10 min */
#define HL_HTTP_STREAM_MAX_LIFE_MS      (24 * 60 * 60 * 1000)   /* 24 h */

typedef struct HlHttpStreamConfig {
    KlHttpServer                 *server;   /* the Keel loop the client runs on */
    struct HlAsyncBackendCtx     *async;    /* timers and the park op's loop    */
    const struct HlAsyncBackend  *backend;  /* NULL = hl_async_backend()        */
    HlHttpConfig *http_cfg;     /* allowlist, TLS, decompression            */
    int     timeout_ms;         /* up to the headers; <= 0 = the app default */
    int     idle_ms;            /* between arrivals; <= 0 = the default      */
    int     life_ms;            /* the whole stream; <= 0 = the default      */
    size_t  buf_cap;            /* unread bytes held; 0 = the default        */
    size_t  max_bytes;          /* whole body; 0 = no limit                  */
    const char *method;
    const char *url;
    const HlHttpHeader *headers;
    int     num_headers;
    const char *body;           /* copied by Keel                            */
    size_t  body_len;
} HlHttpStreamConfig;

/* Start the request. 0 with *out set, or -1 with *out NULL: a bad argument, a
 * reserved header, a host manifest.hosts does not admit (the binding tells it
 * apart with hl_http_url_denied), or a failure to start. Audited like
 * http.fetch (`http.stream`). */
int hl_http_stream_start(HlHttpStream **out, const HlHttpStreamConfig *cfg);

/* The response status and headers: 0 once they arrived, AGAIN before, or the
 * error that ended the stream first. The header array stays valid until free. */
int hl_http_stream_head(HlHttpStream *s, int *status,
                        const HlHttpHeader **headers, int *num_headers);

/* Up to @p n body bytes: > 0 bytes copied, 0 at the end of the body, AGAIN
 * when nothing is buffered yet, or the error that ended the stream (only once
 * the bytes buffered before it were read). */
long hl_http_stream_read(HlHttpStream *s, uint8_t *buf, size_t n);

/* Bytes buffered and not yet read. */
size_t hl_http_stream_buffered(const HlHttpStream *s);

/* The op to park on after an AGAIN (NULL otherwise), and the way back from it. */
struct HlAsyncOp *hl_http_stream_pending_op(HlHttpStream *s);
HlHttpStream *hl_http_stream_from_op(struct HlAsyncOp *op);
void  hl_http_stream_set_user(HlHttpStream *s, void *user);
void *hl_http_stream_user(const HlHttpStream *s);

/* Stop the request and its timers, retract a pending park (silently: the
 * binding wakes its waiter itself). Later calls return E_CLOSED, unless the
 * stream had already ended (its result stands). Idempotent. */
void hl_http_stream_close(HlHttpStream *s);

/* Close (if not yet) and free. NULL is a no-op. */
void hl_http_stream_free(HlHttpStream *s);

/* A short message for an HL_HTTP_STREAM_E_* code; for E_IO, the transport's. */
const char *hl_http_stream_strerror(const HlHttpStream *s, int rc);

/* A stable code for the bindings' error results ("timeout", "idle", ...). */
const char *hl_http_stream_code(int rc);

#endif /* HL_CAP_HTTP_STREAM_H */
