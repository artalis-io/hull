/*
 * http_stream.c - a streamed HTTP response over Keel's async client
 *
 * See include/hull/cap/http_stream.h for the model (push in, pull out, a
 * bounded buffer between, park on AGAIN) and the time and lifetime rules.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/http_stream.h"
#include "hull/cap/audit.h"
#include "hull/limits/http.h"
#include "hull/shared/async_backend.h"

#include <keel/allocator.h>
#include <keel/http_client.h>
#include <keel/http_server.h>
#include <keel/url.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    ST_HEAD = 0,   /* waiting for the response headers           */
    ST_BODY,       /* headers in, body arriving                  */
    ST_END,        /* the response is complete                   */
    ST_ERROR,      /* ended by s->err                            */
    ST_CLOSED      /* closed by the caller before it ended       */
};

struct HlHttpStream {
    const HlAsyncBackend *be;
    HlAsyncBackendCtx    *loop;
    KlHttpClient         *client;    /* NULL once done, cancelled or freed */
    int                   state;
    int                   err;       /* HL_HTTP_STREAM_E_*, in ST_ERROR    */
    KlError               kerr;      /* the transport's error, for E_IO    */

    int                   status;
    HlHttpHeader         *hdrs;      /* owned: array + the strings it names */
    int                   nhdrs;

    uint8_t              *buf;       /* unread bytes are buf[head, head+len) */
    size_t                cap, head, len;
    size_t                max_bytes, total;

    int                   idle_ms;
    uint64_t              last_rx;   /* monotonic ms of the last arrival   */
    int                   paused;    /* the body callback returned PAUSE   */
    uint64_t              head_timer, idle_timer, life_timer;

    struct HlAsyncOp      op;
    int                   op_pending;
    void                 *user;
};

/* Keel keeps the allocator by pointer and reads it when it frees; static, as
 * in http_async.c. */
static KlAllocator    s_alloc;
static pthread_once_t s_alloc_once = PTHREAD_ONCE_INIT;
static void init_alloc(void) { s_alloc = kl_allocator_default(); }
static KlAllocator *get_alloc(void)
{
    pthread_once(&s_alloc_once, init_alloc);
    return &s_alloc;
}

static uint64_t now_ms(const HlHttpStream *s)
{
    return s->be->monotonic_ms ? s->be->monotonic_ms() : 0;
}

/* Complete a pending park once. The op is retracted in close/free, so a
 * resume queued here can never fire into freed storage. */
static void wake(HlHttpStream *s)
{
    if (!s->op_pending) return;
    s->op_pending = 0;
    if (s->be->op_complete) s->be->op_complete(s->loop, &s->op);
}

static void cancel_timers(HlHttpStream *s)
{
    if (s->head_timer) s->be->timer_cancel(s->loop, s->head_timer);
    if (s->idle_timer) s->be->timer_cancel(s->loop, s->idle_timer);
    if (s->life_timer) s->be->timer_cancel(s->loop, s->life_timer);
    s->head_timer = s->idle_timer = s->life_timer = 0;
}

static void drop_client(HlHttpStream *s)
{
    if (!s->client) return;
    kl_http_client_cancel(s->client);
    kl_http_client_free(s->client);
    s->client = NULL;
}

static void audit_end(const char *what, int status, const char *error)
{
    ShJsonWriter w = hl_audit_begin("http.stream");
    sh_json_write_kv_string(&w, "event", what);
    if (status) sh_json_write_kv_int(&w, "status", status);
    if (error) sh_json_write_kv_string(&w, "error", error);
    hl_audit_end(&w);
}

/* End the stream with @p err (first error wins), from a timer: the client is
 * stopped here, since no Keel callback is running. */
static void fail_from_timer(HlHttpStream *s, int err)
{
    if (s->state == ST_HEAD || s->state == ST_BODY) {
        s->state = ST_ERROR;
        s->err = err;
        audit_end("failed", s->status, hl_http_stream_code(err));
    }
    cancel_timers(s);
    drop_client(s);
    wake(s);
}

static void on_head_timer(void *user)
{
    HlHttpStream *s = user;
    s->head_timer = 0;
    if (s->state == ST_HEAD) fail_from_timer(s, HL_HTTP_STREAM_E_TIMEOUT);
}

/* The whole stream's bound, armed at start: it covers the head and the body,
 * so a relay whose client left (Keel does not notice while the handler
 * waits) gives its in-flight slot back at the latest after life_ms. */
static void on_life_timer(void *user)
{
    HlHttpStream *s = user;
    s->life_timer = 0;
    if (s->state == ST_HEAD || s->state == ST_BODY)
        fail_from_timer(s, HL_HTTP_STREAM_E_LIFE);
}

static void on_idle_timer(void *user)
{
    HlHttpStream *s = user;
    s->idle_timer = 0;
    if (s->state != ST_BODY) return;
    uint64_t now = now_ms(s), quiet = now - s->last_rx;
    if (s->paused) quiet = 0;   /* we stopped reading: not the server's gap */
    if (quiet >= (uint64_t)s->idle_ms) {
        fail_from_timer(s, HL_HTTP_STREAM_E_IDLE);
        return;
    }
    /* Something arrived since the timer was armed: wait out the rest. One
     * timer per idle period, not one re-arm per chunk. */
    s->idle_timer = s->be->timer_add(s->loop, (uint64_t)s->idle_ms - quiet,
                                     on_idle_timer, s);
    if (!s->idle_timer) fail_from_timer(s, HL_HTTP_STREAM_E_IO);
}

/* ── Keel callbacks (no app code runs here) ──────────────────────────── */

static int on_headers(int status, const KlHttpClientHeader *headers,
                      int num_headers, void *user)
{
    HlHttpStream *s = user;
    if (s->state != ST_HEAD) return -1;
    if (num_headers < 0) num_headers = 0;

    /* One block: the array, then every name and value. Keel bounded the
     * header section already. */
    size_t bytes = (size_t)num_headers * sizeof(HlHttpHeader);
    for (int i = 0; i < num_headers; i++) {
        size_t a = headers[i].name ? strlen(headers[i].name) : 0;
        size_t b = headers[i].value ? strlen(headers[i].value) : 0;
        if (a > SIZE_MAX / 4 || b > SIZE_MAX / 4 || bytes > SIZE_MAX / 2) goto oom;
        bytes += a + b + 2;
    }
    char *block = malloc(bytes ? bytes : 1);
    if (!block) goto oom;
    HlHttpHeader *arr = (HlHttpHeader *)(void *)block;
    char *p = block + (size_t)num_headers * sizeof(HlHttpHeader);
    for (int i = 0; i < num_headers; i++) {
        const char *n = headers[i].name ? headers[i].name : "";
        const char *v = headers[i].value ? headers[i].value : "";
        size_t a = strlen(n), b = strlen(v);
        memcpy(p, n, a + 1); arr[i].name = p;  p += a + 1;
        memcpy(p, v, b + 1); arr[i].value = p; p += b + 1;
    }
    s->hdrs = arr;
    s->nhdrs = num_headers;
    s->status = status;
    s->state = ST_BODY;
    s->last_rx = now_ms(s);
    if (s->head_timer) s->be->timer_cancel(s->loop, s->head_timer);
    s->head_timer = 0;
    s->idle_timer = s->be->timer_add(s->loop, (uint64_t)s->idle_ms,
                                     on_idle_timer, s);
    if (!s->idle_timer) goto oom;
    audit_end("headers", status, NULL);
    wake(s);
    return 0;
oom:
    s->state = ST_ERROR;
    s->err = HL_HTTP_STREAM_E_IO;
    s->kerr = KL_ERR_ALLOC;
    wake(s);
    return -1;   /* Keel aborts and calls on_done, which frees the client */
}

static int on_body(const char *data, size_t n, void *user)
{
    HlHttpStream *s = user;
    if (s->state != ST_BODY) return KL_HTTP_CLIENT_BODY_ABORT;
    if (n == 0) return KL_HTTP_CLIENT_BODY_CONTINUE;
    int err = 0;
    if (s->max_bytes && (n > s->max_bytes || s->total > s->max_bytes - n))
        err = HL_HTTP_STREAM_E_TOO_LARGE;
    else if (n > s->cap - s->len)
        /* Unreachable while Keel's fragments stay within
         * HL_HTTP_STREAM_FRAGMENT_MAX (we pause before less than that is
         * free). Kept so a larger fragment - a future decoder with a bigger
         * window, a changed receive buffer - ends the stream coded, never
         * writes past the buffer. */
        err = HL_HTTP_STREAM_E_OVERFLOW;
    if (err) {
        s->state = ST_ERROR;
        s->err = err;
        audit_end("failed", s->status, hl_http_stream_code(err));
        wake(s);
        return KL_HTTP_CLIENT_BODY_ABORT;
    }
    if (s->head + s->len + n > s->cap) {   /* room exists, but split: compact */
        memmove(s->buf, s->buf + s->head, s->len);
        s->head = 0;
    }
    memcpy(s->buf + s->head + s->len, data, n);
    s->len += n;
    s->total += n;
    s->last_rx = now_ms(s);
    wake(s);
    /* Room for less than one more fragment: stop Keel reading the socket
     * until a read makes room (hl_http_stream_read resumes it). */
    if (s->cap - s->len < HL_HTTP_STREAM_FRAGMENT_MAX) {
        s->paused = 1;
        return KL_HTTP_CLIENT_BODY_PAUSE;
    }
    return KL_HTTP_CLIENT_BODY_CONTINUE;
}

static void on_done(KlHttpClient *client, void *user)
{
    HlHttpStream *s = user;
    if (kl_http_client_error(client) == 0) {
        if (s->state == ST_HEAD) {
            /* A response with no on_headers (Keel reports none for some
             * shapes): take the status from the final response. */
            const KlHttpClientResponse *r = kl_http_client_response(client);
            s->status = r ? r->status : 0;
        }
        if (s->state == ST_HEAD || s->state == ST_BODY) {
            s->state = ST_END;
            audit_end("end", s->status, NULL);
        }
    } else if (s->state == ST_HEAD || s->state == ST_BODY) {
        s->state = ST_ERROR;
        s->kerr = kl_http_client_last_error(client);
        s->err = s->kerr == KL_ERR_TIMEOUT ? HL_HTTP_STREAM_E_TIMEOUT
                                           : HL_HTTP_STREAM_E_IO;
        audit_end("failed", s->status, kl_strerror(s->kerr));
    }
    cancel_timers(s);
    /* Keel lets the done callback free the client. */
    kl_http_client_free(client);
    s->client = NULL;
    wake(s);
}

/* ── public API ──────────────────────────────────────────────────────── */

int hl_http_stream_start(HlHttpStream **out, const HlHttpStreamConfig *cfg)
{
    if (!out) return -1;
    *out = NULL;
    if (!cfg || !cfg->server || !cfg->async || !cfg->http_cfg ||
        !cfg->method || !cfg->url)
        return -1;
    if (hl_http_reserved_header(cfg->headers, cfg->num_headers)) return -1;
    if (!hl_http_method_valid(cfg->method)) return -1;
    const HlAsyncBackend *be = cfg->backend ? cfg->backend : hl_async_backend();
    if (!be || !be->timer_add || !be->timer_cancel || !be->op_complete ||
        !be->op_cancel)
        return -1;

    KlUrl parsed;
    if (kl_url_parse(cfg->url, &parsed) != 0) return -1;
    if (hl_http_check_host(cfg->http_cfg, parsed.host, parsed.host_len) != 0) {
        ShJsonWriter w = hl_audit_begin("http.stream");
        sh_json_write_kv_string(&w, "method", cfg->method);
        char red[512];
        hl_http_url_redact(cfg->url, red, sizeof red);
        sh_json_write_kv_string(&w, "url", red);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return -1;
    }
    if (parsed.is_https && !cfg->http_cfg->tls) return -1;

    int timeout_ms = hl_http_timeout_resolve(cfg->timeout_ms,
                                             cfg->http_cfg->timeout_ms,
                                             HL_HTTP_FETCH_MAX_TIMEOUT_MS);
    int idle_ms = cfg->idle_ms > 0 ? cfg->idle_ms : HL_HTTP_STREAM_IDLE_DEFAULT_MS;
    if (idle_ms > HL_HTTP_STREAM_IDLE_MAX_MS) idle_ms = HL_HTTP_STREAM_IDLE_MAX_MS;
    int life_ms = cfg->life_ms > 0 ? cfg->life_ms : HL_HTTP_STREAM_LIFE_DEFAULT_MS;
    if (life_ms > HL_HTTP_STREAM_MAX_LIFE_MS) life_ms = HL_HTTP_STREAM_MAX_LIFE_MS;
    size_t cap = cfg->buf_cap ? cfg->buf_cap : HL_HTTP_STREAM_BUF_DEFAULT;
    if (cap < HL_HTTP_STREAM_BUF_MIN) cap = HL_HTTP_STREAM_BUF_MIN;
    if (cap > HL_HTTP_STREAM_BUF_MAX) cap = HL_HTTP_STREAM_BUF_MAX;

    HlHttpStream *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->buf = malloc(cap);
    if (!s->buf) { free(s); return -1; }
    s->be = be;
    s->loop = cfg->async;
    s->cap = cap;
    s->max_bytes = cfg->max_bytes;
    s->idle_ms = idle_ms;

    s->head_timer = be->timer_add(s->loop, (uint64_t)timeout_ms, on_head_timer, s);
    if (!s->head_timer) { free(s->buf); free(s); return -1; }
    s->life_timer = be->timer_add(s->loop, (uint64_t)life_ms, on_life_timer, s);
    if (!s->life_timer) {
        be->timer_cancel(s->loop, s->head_timer);
        free(s->buf);
        free(s);
        return -1;
    }

    KlHttpClientConfig kl_cfg = {
        /* Keel always arms a whole-request deadline; the stream's own are
         * the head, idle and life timers, so Keel's is only a backstop at
         * the largest life. */
        .timeout_ms        = HL_HTTP_STREAM_MAX_LIFE_MS,
        /* Keel caps a streamed body at max_response_size (0 = its default);
         * the stream's caps are buf_cap and max_bytes. */
        .max_response_size = SIZE_MAX / 2,
        .tls               = parsed.is_https ? cfg->http_cfg->tls : NULL,
        .decompress        = cfg->http_cfg->decompress,
        .system_dns        = 1,   /* as http_async.c: the OS resolver */
    };
    KlHttpClientStreamCfg st = {
        .on_body    = on_body,
        .on_headers = on_headers,
        .user_data  = s,
    };
    s->client = kl_http_client_start_s(&cfg->server->ev, get_alloc(), &kl_cfg,
                                       cfg->method, cfg->url,
                                       (const KlHttpClientHeader *)cfg->headers,
                                       cfg->num_headers, cfg->body, cfg->body_len,
                                       &st, on_done, s);
    if (!s->client) {
        cancel_timers(s);
        free(s->buf);
        free(s);
        return -1;
    }

    ShJsonWriter w = hl_audit_begin("http.stream");
    sh_json_write_kv_string(&w, "method", cfg->method);
    char red[512];
    hl_http_url_redact(cfg->url, red, sizeof red);
    sh_json_write_kv_string(&w, "url", red);
    sh_json_write_kv_string(&w, "event", "start");
    hl_audit_end(&w);

    *out = s;
    return 0;
}

int hl_http_stream_head(HlHttpStream *s, int *status,
                        const HlHttpHeader **headers, int *num_headers)
{
    if (!s) return HL_HTTP_STREAM_E_CLOSED;
    if (s->state == ST_HEAD) {
        s->op_pending = 1;
        return HL_HTTP_STREAM_E_AGAIN;
    }
    if (!s->hdrs && s->status == 0) {   /* ended before any headers */
        if (s->state == ST_ERROR) return s->err;
        return HL_HTTP_STREAM_E_CLOSED;
    }
    if (status) *status = s->status;
    if (headers) *headers = s->hdrs;
    if (num_headers) *num_headers = s->nhdrs;
    return HL_HTTP_STREAM_OK;
}

/* A read made room: let Keel read again once a fragment fits and the
 * buffer is at most half full (so a big buffer does not resume per read).
 * Keel only schedules the continuation; a failed schedule leaves the client
 * paused and the next read tries again. */
static void maybe_resume(HlHttpStream *s)
{
    if (!s->paused || !s->client || s->state != ST_BODY) return;
    if (s->cap - s->len < HL_HTTP_STREAM_FRAGMENT_MAX || s->len > s->cap / 2)
        return;
    if (kl_http_client_resume(s->client) != 0) return;
    s->paused = 0;
    s->last_rx = now_ms(s);   /* the idle period starts again here */
}

long hl_http_stream_read(HlHttpStream *s, uint8_t *buf, size_t n)
{
    if (!s) return HL_HTTP_STREAM_E_CLOSED;
    if (s->len > 0) {
        if (!buf || n == 0) return 0;
        size_t k = s->len < n ? s->len : n;
        memcpy(buf, s->buf + s->head, k);
        s->head += k;
        s->len -= k;
        if (s->len == 0) s->head = 0;
        maybe_resume(s);
        return (long)k;
    }
    switch (s->state) {
    case ST_END:    return 0;
    case ST_ERROR:  return s->err;
    case ST_CLOSED: return HL_HTTP_STREAM_E_CLOSED;
    default:
        /* Empty and still paused: only a failed resume leaves it so, and
         * nothing would wake this wait. Try once more, else end the stream. */
        maybe_resume(s);
        if (s->paused && s->state == ST_BODY) {
            s->state = ST_ERROR;
            s->err = HL_HTTP_STREAM_E_IO;
            s->kerr = KL_ERR_ALLOC;
            audit_end("failed", s->status, hl_http_stream_code(s->err));
            cancel_timers(s);
            drop_client(s);
            return s->err;
        }
        s->op_pending = 1;
        return HL_HTTP_STREAM_E_AGAIN;
    }
}

size_t hl_http_stream_buffered(const HlHttpStream *s)
{
    return s ? s->len : 0;
}

int hl_http_stream_client_done(const HlHttpStream *s)
{
    return !s || !s->client;
}

struct HlAsyncOp *hl_http_stream_pending_op(HlHttpStream *s)
{
    return (s && s->op_pending) ? &s->op : NULL;
}

HlHttpStream *hl_http_stream_from_op(struct HlAsyncOp *op)
{
    return op ? (HlHttpStream *)(void *)((char *)op - offsetof(HlHttpStream, op))
              : NULL;
}

void hl_http_stream_set_user(HlHttpStream *s, void *user)
{
    if (s) s->user = user;
}

void *hl_http_stream_user(const HlHttpStream *s)
{
    return s ? s->user : NULL;
}

void hl_http_stream_close(HlHttpStream *s)
{
    if (!s) return;
    if (s->state == ST_HEAD || s->state == ST_BODY) {
        s->state = ST_CLOSED;
        audit_end("closed", s->status, NULL);
    }
    cancel_timers(s);
    drop_client(s);
    if (s->loop && s->be->op_cancel) s->be->op_cancel(s->loop, &s->op);
    s->op_pending = 0;
    s->len = 0;
}

void hl_http_stream_free(HlHttpStream *s)
{
    if (!s) return;
    hl_http_stream_close(s);
    free(s->hdrs);
    free(s->buf);
    free(s);
}

const char *hl_http_stream_code(int rc)
{
    switch (rc) {
    case HL_HTTP_STREAM_E_TIMEOUT:   return "timeout";
    case HL_HTTP_STREAM_E_IDLE:      return "idle";
    case HL_HTTP_STREAM_E_OVERFLOW:  return "overflow";
    case HL_HTTP_STREAM_E_TOO_LARGE: return "too_large";
    case HL_HTTP_STREAM_E_CLOSED:    return "closed";
    case HL_HTTP_STREAM_E_LIFE:      return "life";
    case HL_HTTP_STREAM_E_IO:        return "io_error";
    default:                         return "error";
    }
}

const char *hl_http_stream_strerror(const HlHttpStream *s, int rc)
{
    switch (rc) {
    case HL_HTTP_STREAM_E_TIMEOUT:
        return "no response headers within the timeout";
    case HL_HTTP_STREAM_E_IDLE:
        return "nothing arrived within the idle timeout";
    case HL_HTTP_STREAM_E_OVERFLOW:
        return "a body fragment did not fit the stream's buffer";
    case HL_HTTP_STREAM_E_TOO_LARGE:
        return "the response body passed max_bytes";
    case HL_HTTP_STREAM_E_CLOSED:
        return "the stream is closed";
    case HL_HTTP_STREAM_E_LIFE:
        return "the stream outlived its life limit";
    case HL_HTTP_STREAM_E_IO:
        return (s && s->kerr) ? kl_strerror(s->kerr) : "transport error";
    default:
        return "stream error";
    }
}
