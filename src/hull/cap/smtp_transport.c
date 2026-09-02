/*
 * smtp_transport.c: SMTP byte transport over Keel v3 public primitives.
 *
 * See include/hull/cap/smtp_transport.h for the ownership split and contract.
 *
 * This composes Keel's PUBLIC transport surface directly (the reviewed Slice-2a
 * spike pattern, PR #447): KlConnectOp (resolve + Happy-Eyeballs racing connect),
 * KlStream (bounded queued writes, strict read pause/resume, graceful close),
 * KlTls (implicit TLS + in-place STARTTLS), timers, and the POSIX socket
 * provider's public ops table - all on ONE private, operation-local KlEventCtx
 * that the synchronous SMTP capability pumps to a terminal state (design
 * section 6, model 1).
 *
 * NO vendor/keel/src header is used. The two *_detail.h headers are included
 * solely so KlConnectOp / KlStream can be EMBEDDED fields (storage sizing); their
 * struct fields are never read or written (design section 4.4).
 *
 * Two invariants carried from the Slice-2a spike (design section 9):
 *   INVARIANT 1 - the KlAllocator handed to KlTls / KlTlsCtx must OUTLIVE every
 *     TLS object (they capture it by pointer and dereference at destroy). We use
 *     a persistent process-wide allocator (persistent_tls_alloc), mirroring
 *     src/hull/shared/tls_client.c. A stack-local would dangle.
 *   INVARIANT 2 - at STARTTLS, transfer socket ownership to the TLS session
 *     BEFORE any plaintext read can consume ClientHello bytes: pause the
 *     plaintext read side first, then hand the fd to a dedicated handshake
 *     watcher. A plaintext read that pulled the ClientHello would steal it from
 *     mbedTLS and deadlock the handshake.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/smtp_transport.h"
#include "hull/cap/smtp.h"        /* hl_smtp_parse_response (the one code parser) */
#include "hull/limits/core.h"     /* HL_SMTP_RECV_BUF_SIZE */

/* PUBLIC Keel headers only (mirrors the spike compile gate). */
#include <keel/connect_op.h>
#include <keel/connect_op_detail.h> /* opt-in layout: embed a KlConnectOp (storage only) */
#include <keel/stream.h>
#include <keel/stream_detail.h>     /* opt-in layout: embed a KlStream (storage only) */
#include <keel/event_ctx.h>
#include <keel/event.h>
#include <keel/socket.h>
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include <keel/timer.h>
#include <keel/tls.h>
#include <keel/error.h>
#include <keel/allocator.h>
#include <keel/clock.h>   /* kl_monotonic_ms: monotonic per-stage deadlines */
#include <keel_tls_mbedtls.h>

/* Resolution is a sandbox-compatible BLOCKING getaddrinfo, kept out of
 * cap/smtp.c (which may contain no getaddrinfo/socket/poll/read/write/close).
 * This preserves the current SMTP behavior: /etc/hosts, search domains, and the
 * OS resolver the kernel-sandbox network-outbound grant permits - the same
 * reason cap/http_async.c forces system_dns for the async HTTP client. */
#include <netdb.h>
#include <sys/socket.h>   /* AF_UNSPEC / AF_INET / AF_INET6 (cosmo needs it explicit) */
#include <netinet/in.h>   /* struct sockaddr_in / sockaddr_in6 for the resolver */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

/* ────────────────────────────────────────────────────────────────────────────
 * Persistent process-wide pieces (borrowed, not per-op).
 * ────────────────────────────────────────────────────────────────────────── */

/* The POSIX socket provider: an immutable process-wide ops table + context. We
 * call its PUBLIC ops directly (the documented authoring API), reading errno for
 * would-block / EINPROGRESS classification (io_status is NULL on the POSIX
 * provider: the defined hosted-errno fallback). */
static const KlSocketProvider *g_sp;
static pthread_once_t          g_sp_once = PTHREAD_ONCE_INIT;
static void init_sp(void) { g_sp = kl_socket_provider_posix(); }
static const KlSocketProvider *socket_provider(void)
{
    pthread_once(&g_sp_once, init_sp);
    return g_sp;
}

static KlSocketHandle sp_socket(int domain, int type, int protocol)
{ return g_sp->ops->socket(g_sp->context, domain, type, protocol); }
static int sp_set_nonblocking(KlSocketHandle fd)
{ return g_sp->ops->set_nonblocking(g_sp->context, fd); }
static int sp_connect(KlSocketHandle fd, const KlSockAddr *a)
{ return g_sp->ops->connect(g_sp->context, fd, a); }
static int sp_get_so_error(KlSocketHandle fd, int *out)
{ return g_sp->ops->get_so_error(g_sp->context, fd, out); }
static kl_ssize_t sp_send(KlSocketHandle fd, const void *b, size_t n)
{ return g_sp->ops->send(g_sp->context, fd, b, n); }
static kl_ssize_t sp_recv(KlSocketHandle fd, void *b, size_t n)
{ return g_sp->ops->recv(g_sp->context, fd, b, n); }
static int sp_close(KlSocketHandle fd)
{ return g_sp->ops->close(g_sp->context, fd); }

static int errno_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }

/* INVARIANT 1: PERSISTENT allocator for KlTls / KlTlsCtx. Keel captures the
 * KlAllocator BY POINTER and dereferences it at destroy time - long after the
 * creating call returns - so it must outlive every TLS object. Hold ONE copy in
 * static storage (as src/hull/shared/tls_client.c does), never a stack local. */
static KlAllocator    s_tls_alloc;
static pthread_once_t s_tls_alloc_once = PTHREAD_ONCE_INIT;
static void init_tls_alloc(void) { s_tls_alloc = kl_allocator_default(); }
static KlAllocator *persistent_tls_alloc(void)
{
    pthread_once(&s_tls_alloc_once, init_tls_alloc);
    return &s_tls_alloc;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Incremental SMTP reply parser.
 *
 * The KlStream deliver callback does not map 1:1 to SMTP lines: bytes arrive
 * fragmented and/or coalesced. This accumulator retains partial bytes across
 * delivers, then, on demand, scans complete CRLF-terminated lines. A reply is
 * complete when a terminating line (code + ' ') is seen; continuation lines
 * (code + '-') keep it open. All continuation lines must carry the SAME 3-digit
 * code. A single line and the whole multiline response are bounded.
 * ────────────────────────────────────────────────────────────────────────── */

#define SMTP_MAX_LINE     HL_SMTP_RECV_BUF_SIZE   /* 1024: one reply line cap */
#define SMTP_MAX_REPLY    8192                     /* whole multiline response cap */

/* Bounded chunked write admission (fix: message-size regression). The KlStream
 * write queue is fixed at SMTP_WRITE_QUEUE_BYTES; a formatted message may be up
 * to HL_SMTP_MAX_MSG_SIZE (10 MiB), and kl_stream_write is atomic (a single
 * write larger than the queue returns KL_STREAM_TOO_LARGE). So the write path
 * admits the payload in SMTP_WRITE_CHUNK-sized pieces, draining the queue
 * between chunks, keeping total queued memory bounded (never enlarging the queue
 * to 10 MiB). */
#define SMTP_WRITE_QUEUE_BYTES  (4 * 1024 * 1024)   /* KlStream write-queue size */
#define SMTP_WRITE_CHUNK        (256 * 1024)        /* per-admission chunk */

/* RFC 8305 Connection Attempt Delay: how long to wait before starting the next
 * address while an earlier attempt is still in flight (the Happy Eyeballs
 * stagger). ~250 ms is the RFC-recommended default. */
#define SMTP_CONNECT_ATTEMPT_DELAY_MS  250
/* Max wait for a GRACEFUL close (drain + close_notify) before hl_smtp_transport_
 * free's abortive cancel takes over; a healthy close completes near-instantly, a
 * dead/stalled peer must not block the synchronous caller for the op timeout. */
#define SMTP_CLOSE_GRACE_MS            2000

typedef struct {
    char   buf[SMTP_MAX_REPLY + SMTP_MAX_LINE];  /* accumulator (+1 line of slack) */
    size_t len;
    int    overflow;   /* set once bytes exceeded the bound: fail closed */
} SmtpReplyAcc;

static void reply_acc_reset(SmtpReplyAcc *a) { a->len = 0; a->overflow = 0; a->buf[0] = '\0'; }

/* Append delivered bytes. Fails closed (overflow=1) past the bound. */
static void reply_acc_push(SmtpReplyAcc *a, const char *data, size_t n)
{
    if (a->overflow) return;
    if (n > sizeof a->buf - 1 - a->len) {
        a->overflow = 1;
        return;
    }
    memcpy(a->buf + a->len, data, n);
    a->len += n;
    a->buf[a->len] = '\0';
}

/*
 * Try to extract one complete reply from the accumulator.
 *
 * Returns:
 *   >0  the 3-digit code, and copies the raw reply text (all its lines) into
 *       @p out (NUL-terminated, up to @p out_size-1). Consumed bytes are dropped
 *       from the accumulator front (pipelined bytes, if any, are retained).
 *   0   incomplete: need more bytes.
 *  -1   malformed / over-limit / inconsistent continuation code: fail closed.
 */
static int reply_acc_take(SmtpReplyAcc *a, char *out, int out_size)
{
    if (a->overflow)
        return -1;

    size_t scan = 0;    /* bytes consumed so far this reply */
    int    code = -1;

    for (;;) {
        /* Find the next CRLF from `scan`. */
        char *nl = memchr(a->buf + scan, '\n', a->len - scan);
        if (!nl)
            return 0;   /* no complete line yet */
        size_t line_len = (size_t)(nl - (a->buf + scan)) + 1;  /* incl '\n' */

        /* A single line is bounded (must be CRLF-terminated and short). */
        if (line_len > SMTP_MAX_LINE)
            return -1;
        if (line_len < 2 || a->buf[scan + line_len - 2] != '\r')
            return -1;   /* not CRLF-terminated */

        /* Whole-response bound BEFORE accepting THIS line (including the
         * terminal one): a reply must never exceed SMTP_MAX_REPLY on ANY line.
         * (The old code only bounded continuation lines, so a terminal reply
         * could grow past the cap.) */
        if (scan + line_len > SMTP_MAX_REPLY)
            return -1;

        int line_code = hl_smtp_parse_response(a->buf + scan, (int)line_len);
        if (line_code < 0)
            return -1;

        if (code < 0) {
            code = line_code;
        } else if (line_code != code) {
            return -1;   /* continuation code must match (RFC 5321) */
        }

        /* The 4th char decides continuation vs termination and must be EXACTLY:
         *   NNN-       -> continuation
         *   NNN<space> -> termination
         *   NNN\r\n    -> termination (a bare 3-digit line; line_len == 5)
         * Any other 4th char (e.g. "250X...") is malformed: fail closed. */
        int is_continuation;
        if (line_len == 5) {
            /* Exactly "NNN\r\n": a bare code line terminates. */
            is_continuation = 0;
        } else {
            char sep = a->buf[scan + 3];
            if (sep == '-')
                is_continuation = 1;
            else if (sep == ' ')
                is_continuation = 0;
            else
                return -1;   /* malformed 4th character */
        }
        scan += line_len;

        if (!is_continuation) {
            /* Complete reply: copy [0, scan) into out, drop it from the front. */
            size_t total = scan;
            if (out && out_size > 0) {
                size_t cp = total;
                if (cp > (size_t)out_size - 1) cp = (size_t)out_size - 1;
                memcpy(out, a->buf, cp);
                out[cp] = '\0';
            }
            memmove(a->buf, a->buf + total, a->len - total);
            a->len -= total;
            a->buf[a->len] = '\0';
            return code;
        }
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Watcher ownership. One watcher per fd carries a READ and/or WRITE interest
 * mask; we track it ourselves and drive add/mod/del, failing closed on a real
 * backend error (never masking an add failure as "already armed").
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct { int armed; KlEventMask mask; } WatcherState;

/* ────────────────────────────────────────────────────────────────────────────
 * The transport operation.
 * ────────────────────────────────────────────────────────────────────────── */

struct HlSmtpTransport {
    KlEventCtx    ev;
    int           ev_ready;      /* 1 once kl_event_ctx_init succeeded */
    KlAllocator   alloc;         /* op-local write-queue allocator (loop lifetime) */

    /* Cancellation predicate, checked by every pump before + after each event-loop
     * run so a requested cancel (shutdown pass 1, per-request teardown, deadline)
     * aborts the in-flight conversation into confirmed teardown within one step
     * (~50 ms) instead of waiting for the stage timeout. Set once, after DNS, by
     * hl_smtp_transport_connect; NULL on the sync no-loop path. Blocking DNS is the
     * documented non-interruptible exception (it runs before this is set). */
    int          (*cancel_poll)(void *user);
    void          *cancel_user;

    KlConnectOp   connect_op;    /* embedded (connect_op_detail.h: storage only) */
    int           connect_started;  /* 1 once kl_connect_op_start succeeded */
    KlStream      stream;        /* embedded (stream_detail.h: storage only) */
    int           stream_up;     /* 1 once the stream was brought up */
    int           write_q_inited;/* 1 once kl_stream_write_init succeeded (fix 8) */

    /* Resolution: a system-resolved address list KlConnectOp races. */
    KlSockAddr    addrs[KL_CONNECT_MAX_ADDRS];
    int           naddrs;
    KlSocketHandle attempt_fd[KL_CONNECT_MAX_ADDRS];
    WatcherState  conn_ws[KL_CONNECT_MAX_ADDRS];

    KlSocketHandle fd;           /* the winning connected fd */
    WatcherState   stream_ws;    /* READ|WRITE interest on the winning fd */

    /* Connect outcome. */
    int             connect_done;    /* on_done fired */
    int             connect_detached;/* on_detach fired */
    KlConnectResult connect_result;
    int             connect_error;

    /* Overall connect deadline. */
    int64_t       deadline_timer;
    uint64_t      deadline_ms;
    int           deadline_fired;

    /* FROZEN post-resolution operation deadline (Dop, section 8): an ABSOLUTE
     * monotonic instant set ONCE right after resolution. Every post-resolution
     * stage bounds by Dstage = min(Dop, now + stage_budget), so no stage or retry
     * can extend Dop. dop_expired is set by any pump that finds now >= Dop, so the
     * caller can tag terminal:post_resolution_deadline (public token unchanged). */
    uint64_t      dop_ms;
    int           dop_expired;

    /* RFC 8305 Connection Attempt Delay timer (the Happy Eyeballs stagger). */
    int64_t       delay_timer;

    /* Read side: stable KlStream buffer + the incremental reply accumulator. */
    char          read_buf[4096];
    SmtpReplyAcc  acc;
    int           read_eof;      /* 1 once a terminal EOF/error was delivered */

    /* Write side: track drain-completion for the synchronous write pump. */
    int           write_error;   /* 1 if the writer reported a fatal error */

    /* Close lifecycle. */
    int           close_begun;
    int           closed;        /* on_close fired */

    /* TLS (live). The KlTlsCtx is caller-owned (cfg->ctx); this transport owns
     * only the per-session KlTls, so no ctx pointer is retained here. */
    KlTls        *tls;
    int           tls_up;        /* verified session active */
    int           tls_handshaking;
    int           tls_done;      /* handshake reached a terminal (ok or fail) */
    int           tls_failed;    /* handshake failed */
    int           hs_watch_armed;
};

/* forward decls */
static void tp_read_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data);
static void tp_tls_handshake_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data);
static void tp_tls_drive_handshake(HlSmtpTransport *t);

/* ── watcher helpers ─────────────────────────────────────────────────────── */

static int stream_watch_set(HlSmtpTransport *t, KlEventMask mask)
{
    if (!t->stream_ws.armed) {
        if (kl_watcher_add(&t->ev, t->fd, mask, tp_read_watcher, t) != 0)
            return -1;
        t->stream_ws.armed = 1;
        t->stream_ws.mask  = mask;
        return 0;
    }
    if (t->stream_ws.mask == mask)
        return 0;
    if (kl_watcher_mod(&t->ev, t->fd, mask) != 0)
        return -1;
    t->stream_ws.mask = mask;
    return 0;
}

static void stream_watch_clear(HlSmtpTransport *t)
{
    if (t->stream_ws.armed) {
        kl_watcher_del(&t->ev, t->fd);
        t->stream_ws.armed = 0;
        t->stream_ws.mask  = 0;
    }
}

/* ── KlStream WRITE facet ────────────────────────────────────────────────── */
/* Readiness writer over the provider send op (or tls->write). On would-block /
 * partial write, arm a WRITABLE watcher so kl_stream_flush drains the queue on
 * write-readiness (backpressure). Fail closed on a real watcher error. */
static kl_ssize_t tp_stream_write(const char *data, size_t len, void *ctx)
{
    HlSmtpTransport *t = ctx;
    kl_ssize_t n;
    if (t->tls_up) {
        n = t->tls->write(t->tls, t->fd, data, len);
        if (n > 0) return n;
        if (n == 0) {   /* WANT_WRITE */
            if (stream_watch_set(t, KL_EVENT_READ | KL_EVENT_WRITE) != 0)
                return -1;
            return 0;
        }
        return -1;
    }
    n = sp_send(t->fd, data, len);
    if (n < 0) {
        if (errno_would_block()) {
            if (stream_watch_set(t, KL_EVENT_READ | KL_EVENT_WRITE) != 0)
                return -1;
            return 0;
        }
        return -1;
    }
    if ((size_t)n < len) {   /* partial: send buffer full - re-arm WRITE */
        if (stream_watch_set(t, KL_EVENT_READ | KL_EVENT_WRITE) != 0)
            return -1;
    }
    return n;
}

/* ── KlStream READ facet ─────────────────────────────────────────────────── */

static void tp_stream_deliver(void *ctx, const char *buf, size_t len, int ok)
{
    HlSmtpTransport *t = ctx;
    if (!ok) { t->read_eof = 1; return; }     /* terminal EOF/error on read side */
    if (len > 0)
        reply_acc_push(&t->acc, buf, len);
}

static int tp_stream_read_arm(void *ctx)
{
    HlSmtpTransport *t = ctx;
    KlEventMask m = KL_EVENT_READ;
    if (t->stream_ws.armed && (t->stream_ws.mask & KL_EVENT_WRITE))
        m |= KL_EVENT_WRITE;
    return stream_watch_set(t, m);
}

static void tp_stream_read_disarm(void *ctx)
{
    HlSmtpTransport *t = ctx;
    if (t->stream_ws.armed && (t->stream_ws.mask & KL_EVENT_WRITE)) {
        if (kl_watcher_mod(&t->ev, t->fd, KL_EVENT_WRITE) == 0)
            t->stream_ws.mask = KL_EVENT_WRITE;
    } else {
        stream_watch_clear(t);
    }
}

/* The connected fd is ready: WRITE -> drain the queue; READ -> pull bytes. */
static void tp_read_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    HlSmtpTransport *t = user_data;

    if (ready & KL_EVENT_WRITE) {
        int fl = kl_stream_flush(&t->stream);
        if (fl < 0) t->write_error = 1;
        if (kl_stream_write_pending(&t->stream) == 0 && t->stream_ws.armed) {
            if (kl_watcher_mod(&t->ev, t->fd, KL_EVENT_READ) == 0)
                t->stream_ws.mask = KL_EVENT_READ;
        }
    }

    if (ready & KL_EVENT_READ) {
        if (t->tls_up && t->tls) {
            for (;;) {
                kl_ssize_t n = t->tls->read(t->tls, fd, t->read_buf, sizeof t->read_buf);
                if (n > 0) {
                    kl_stream_on_recv(&t->stream, (size_t)n, 1);
                    if (!t->tls_up || !t->tls)
                        return;   /* teardown began under deliver */
                } else if (n == 0) {
                    break;        /* WANT_READ */
                } else {
                    kl_stream_on_recv(&t->stream, 0, 0);
                    return;
                }
                if (t->tls->pending && t->tls->pending(t->tls) == 0)
                    break;
            }
            return;
        }
        kl_ssize_t n = sp_recv(fd, t->read_buf, sizeof t->read_buf);
        if (n > 0) {
            kl_stream_on_recv(&t->stream, (size_t)n, 1);
        } else if (n == 0) {
            kl_stream_on_recv(&t->stream, 0, 0);
        } else {
            if (errno_would_block())
                return;
            kl_stream_on_recv(&t->stream, 0, 0);
        }
    }
}

/* ── KlStream CLOSE facet ────────────────────────────────────────────────── */
static void tp_stream_on_close(void *ctx)
{
    HlSmtpTransport *t = ctx;
    t->closed = 1;
    if (t->tls) {
        if (t->tls_up && t->tls->shutdown)
            t->tls->shutdown(t->tls, t->fd);
        t->tls->destroy(t->tls);
        t->tls = NULL;
        t->tls_up = 0;
    }
    if (kl_handle_valid(t->fd)) {
        stream_watch_clear(t);
        sp_close(t->fd);
        t->fd = KL_INVALID_SOCKET;
    }
}

static int tp_stream_bringup(HlSmtpTransport *t)
{
    if (kl_stream_init(&t->stream, t->read_buf, sizeof t->read_buf) != 0)
        return -1;
    /* Fix 8: record the write-queue init INDEPENDENTLY of stream_up. If a later
     * facet init fails below, stream_up stays 0 but the write queue is already
     * allocated; teardown frees it iff write_q_inited (never leaks a partial
     * bring-up). */
    if (kl_stream_write_init(&t->stream, &t->alloc, SMTP_WRITE_QUEUE_BYTES) != 0)
        return -1;
    t->write_q_inited = 1;
    if (kl_stream_set_writer(&t->stream, tp_stream_write, t) != 0)
        return -1;
    if (kl_stream_read_init(&t->stream, /*completion_mode=*/0,
                            tp_stream_deliver, tp_stream_read_arm,
                            tp_stream_read_disarm, t) != 0)
        return -1;
    if (kl_stream_close_init(&t->stream, tp_stream_on_close, t) != 0)
        return -1;
    if (kl_stream_read_start(&t->stream) != 0)
        return -1;
    t->stream_up = 1;
    return 0;
}

/* ────────────────────────────────────────────────────────────────────────────
 * LIVE TLS handshake, driven off the private event loop.
 * ────────────────────────────────────────────────────────────────────────── */
static int tp_tls_begin_handshake(HlSmtpTransport *t, const char *host, void *tls_cfg)
{
    KlTlsConfig *cfg = (KlTlsConfig *)tls_cfg;
    if (!cfg || !cfg->factory)
        return -1;

    /* INVARIANT 1: persistent allocator. The per-session KlTls is created from
     * the caller-owned KlTlsCtx (cfg->ctx) via cfg->factory, exactly as
     * shared/tls_client.c's cfg path does - so the CA bundle + verification
     * policy wired by serve.c apply. We own only the KlTls, not the ctx. */
    t->tls = cfg->factory(cfg->ctx, persistent_tls_alloc());
    if (!t->tls)
        return -1;
    /* Validate the returned vtable right after the factory. An invalid vtable
     * (a backend missing a required op) must abort, not be driven. A NULL
     * `destroy` is one reason kl_tls_vtable_valid fails, so the destroy call on
     * THIS path must be NULL-checked (Keel's contract requires it): calling a
     * NULL destroy on a malformed vtable would crash during rejection. */
    if (!kl_tls_vtable_valid(t->tls)) {
        if (t->tls->destroy)
            t->tls->destroy(t->tls);
        t->tls = NULL;
        return -1;
    }
    /* set_hostname is REQUIRED for SNI + certificate/hostname verification: for a
     * non-empty SMTP host, require BOTH that the hook exists AND that it
     * succeeds. Proceeding without it (an absent hook) would handshake without
     * the expected identity and defeat verify-full. (Past the vtable check
     * destroy is non-NULL, but keep the guard for consistency.) */
    if (host && host[0]) {
        if (!t->tls->set_hostname || t->tls->set_hostname(t->tls, host) != 0) {
            if (t->tls->destroy)
                t->tls->destroy(t->tls);
            t->tls = NULL;
            return -1;
        }
    }

    t->tls_handshaking = 1;
    t->tls_done = 0;
    t->tls_failed = 0;

    /* INVARIANT 2: the plaintext read side is already paused by the caller; hand
     * the fd to a DEDICATED handshake watcher so mbedTLS reads the socket itself
     * (the ClientHello is never pulled into a plaintext buffer). Client writes
     * ClientHello first, so arm WRITE; the drive loop re-arms per WANT_*. */
    stream_watch_clear(t);
    if (kl_watcher_add(&t->ev, t->fd, KL_EVENT_WRITE, tp_tls_handshake_watcher, t) != 0)
        return -1;
    t->hs_watch_armed = 1;
    return 0;
}

static void tp_tls_handshake_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    (void)fd; (void)ready;
    tp_tls_drive_handshake(user_data);
}

static void tp_tls_drive_handshake(HlSmtpTransport *t)
{
    KlTlsResult r = t->tls->handshake(t->tls, t->fd);
    if (r == KL_TLS_OK) {
        t->tls_handshaking = 0;
        t->tls_done = 1;
        if (t->hs_watch_armed) {
            kl_watcher_del(&t->ev, t->fd);
            t->hs_watch_armed = 0;
        }
        t->tls_up = 1;
        /* Hand the fd back to the stream read watcher (now via tls->read/write)
         * and resume the raw read side. Fix 7: if EITHER the watcher re-arm OR
         * the stream resume fails, the stream is not reattached - do NOT report
         * TLS success; fail closed with an abortive close. */
        t->stream_ws.armed = 0;
        t->stream_ws.mask  = 0;
        if (stream_watch_set(t, KL_EVENT_READ) != 0 ||
            kl_stream_resume(&t->stream) != 0) {
            t->tls_up = 0;
            t->tls_failed = 1;
            kl_stream_cancel(&t->stream);
            return;
        }
        return;
    }
    if (r == KL_TLS_ERROR) {
        t->tls_handshaking = 0;
        t->tls_done = 1;
        t->tls_failed = 1;
        if (t->hs_watch_armed) {
            kl_watcher_del(&t->ev, t->fd);
            t->hs_watch_armed = 0;
            t->stream_ws.armed = 0;
            t->stream_ws.mask  = 0;
        }
        /* NO plaintext fallback: fail closed with an abortive stream close. */
        kl_stream_cancel(&t->stream);
        return;
    }
    /* WANT_READ / WANT_WRITE: re-arm the dedicated handshake watcher. */
    KlEventMask want = (r == KL_TLS_WANT_READ) ? KL_EVENT_READ : KL_EVENT_WRITE;
    if (kl_watcher_mod(&t->ev, t->fd, want) != 0) {
        t->tls_handshaking = 0;
        t->tls_done = 1;
        t->tls_failed = 1;
        if (t->hs_watch_armed) {
            kl_watcher_del(&t->ev, t->fd);
            t->hs_watch_armed = 0;
            t->stream_ws.armed = 0;
            t->stream_ws.mask  = 0;
        }
        kl_stream_cancel(&t->stream);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * KlConnectOp hooks (bring-your-own I/O).
 *
 * Resolution is a sandbox-compatible BLOCKING getaddrinfo that completes
 * synchronously inside start_resolve (the connect op explicitly supports inline
 * completion). It resolves the DECLARED host and yields the address list to
 * KlConnectOp to race - never widening authority (authorization already ran
 * against the hostname before connect).
 * ────────────────────────────────────────────────────────────────────────── */

static int co_start_resolve(void *ctx);
static void co_connect_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data);

/* Resolve host:port into t->addrs via getaddrinfo. Returns naddrs (>=1) or 0. */
static int resolve_addrs(HlSmtpTransport *t, const char *host, int port)
{
    char port_str[8];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return 0;

    int n = 0;
    for (struct addrinfo *ai = res; ai && n < KL_CONNECT_MAX_ADDRS; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET && ai->ai_addr) {
            const struct sockaddr_in *s = (const struct sockaddr_in *)(const void *)ai->ai_addr;
            uint8_t ip[4];
            memcpy(ip, &s->sin_addr, 4);
            if (kl_sockaddr_from_ipv4(&t->addrs[n], ip, (uint16_t)port) == 0)
                n++;
        } else if (ai->ai_family == AF_INET6 && ai->ai_addr) {
            const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)ai->ai_addr;
            uint8_t ip6[16];
            memcpy(ip6, &s6->sin6_addr, 16);
            if (kl_sockaddr_from_ipv6(&t->addrs[n], ip6, (uint16_t)port,
                                      s6->sin6_scope_id) == 0)
                n++;
        }
    }
    freeaddrinfo(res);
    return n;
}

static int co_start_resolve(void *ctx)
{
    HlSmtpTransport *t = ctx;
    /* t->naddrs was filled by resolve_addrs before start (synchronous, inline). */
    if (t->naddrs < 1) {
        kl_connect_op_on_resolve_failed(&t->connect_op, (int)KL_ERR_DNS);
        return 0;
    }
    kl_connect_op_on_resolved(&t->connect_op, t->naddrs);
    return 0;
}

static int co_start_attempt(void *ctx, int idx, int *out_err)
{
    HlSmtpTransport *t = ctx;
    if (idx < 0 || idx >= t->naddrs) { *out_err = (int)KL_ERR_CONNECT; return -1; }

    KlSocketHandle fd = sp_socket(t->addrs[idx].family == KL_AF_INET6 ? AF_INET6
                                                                      : AF_INET,
                                  SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) { *out_err = (int)KL_ERR_CONNECT; return -1; }
    if (sp_set_nonblocking(fd) < 0) { sp_close(fd); *out_err = (int)KL_ERR_CONNECT; return -1; }

    int rc = sp_connect(fd, &t->addrs[idx]);
    int ce = errno;
    if (rc < 0 && ce != EINPROGRESS) {
        sp_close(fd);
        *out_err = (int)KL_ERR_CONNECT;
        return -1;   /* hard local failure: advance to the next address */
    }
    if (kl_watcher_add(&t->ev, fd, KL_EVENT_WRITE, co_connect_watcher, t) != 0) {
        sp_close(fd);
        *out_err = (int)KL_ERR_CONNECT;
        return -1;
    }
    t->attempt_fd[idx] = fd;
    t->conn_ws[idx].armed = 1;
    t->conn_ws[idx].mask  = KL_EVENT_WRITE;

    if (rc == 0) {   /* connected immediately (loopback often does) */
        kl_watcher_del(&t->ev, fd);
        t->conn_ws[idx].armed = 0;
        t->attempt_fd[idx] = KL_INVALID_SOCKET;
        kl_connect_op_on_attempt_connected(&t->connect_op, idx, fd);
    }
    return 0;
}

static void co_connect_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    HlSmtpTransport *t = user_data;
    (void)ready;
    int idx = -1;
    for (int i = 0; i < t->naddrs; i++)
        if (t->attempt_fd[i] == fd) { idx = i; break; }
    if (idx < 0) return;

    int soerr = 0;
    int gsrc = sp_get_so_error(fd, &soerr);
    kl_watcher_del(&t->ev, fd);
    t->conn_ws[idx].armed = 0;
    if (gsrc == 0 && soerr == 0) {
        t->attempt_fd[idx] = KL_INVALID_SOCKET;   /* ownership moves to the op */
        kl_connect_op_on_attempt_connected(&t->connect_op, idx, fd);
    } else {
        sp_close(fd);
        t->attempt_fd[idx] = KL_INVALID_SOCKET;
        kl_connect_op_on_attempt_failed(&t->connect_op, idx, (int)KL_ERR_CONNECT);
    }
}

static void co_cancel_attempt(void *ctx, int idx)
{
    HlSmtpTransport *t = ctx;
    if (idx >= 0 && idx < t->naddrs && kl_handle_valid(t->attempt_fd[idx])) {
        if (t->conn_ws[idx].armed) {
            kl_watcher_del(&t->ev, t->attempt_fd[idx]);
            t->conn_ws[idx].armed = 0;
        }
        sp_close(t->attempt_fd[idx]);
        t->attempt_fd[idx] = KL_INVALID_SOCKET;
    }
    kl_connect_op_on_attempt_failed(&t->connect_op, idx, (int)KL_ERR_CONNECT);
}

static void co_dispose_fd(void *ctx, KlSocketHandle fd)
{
    (void)ctx;
    if (kl_handle_valid(fd))
        sp_close(fd);
}

static void co_on_deadline_fired(void *user_data)
{
    HlSmtpTransport *t = user_data;
    t->deadline_timer = -1;
    t->deadline_fired = 1;
    /* The connect-deadline timer is armed with Dop - now (section 8, co_arm_
     * deadline), so its firing IS a post-resolution deadline expiry. Classify it
     * here: the connect pump exits via done_connect (connect failed), not the
     * pump's own deadline branch, so this is where the connect-phase Dop is tagged
     * (surfaced via out_dop_expired since a failed connect frees the transport). */
    t->dop_expired = 1;
    kl_connect_op_on_deadline(&t->connect_op, (int)KL_ERR_TIMEOUT);
}

/* ── RFC 8305 Connection Attempt Delay (Happy Eyeballs stagger) ─────────────
 * arm_delay schedules a ~250 ms timer while an attempt is in flight; on fire it
 * tells the connect op to start the NEXT address (racing), rather than waiting
 * for the earlier attempt to fail (sequential fallback). cancel_delay retires
 * the timer synchronously (required by the KlConnectOp contract when arm_delay
 * is set). */
static void co_on_delay_fired(void *user_data)
{
    HlSmtpTransport *t = user_data;
    t->delay_timer = -1;
    kl_connect_op_on_delay(&t->connect_op);
}
static int co_arm_delay(void *ctx)
{
    HlSmtpTransport *t = ctx;
    t->delay_timer = kl_timer_add(&t->ev, SMTP_CONNECT_ATTEMPT_DELAY_MS,
                                  co_on_delay_fired, t);
    if (t->delay_timer < 0)
        return -1;   /* arm failed: the machine fast-starts the next address */
    return 0;
}
static void co_cancel_delay(void *ctx)
{
    HlSmtpTransport *t = ctx;
    if (t->delay_timer >= 0) {
        kl_timer_cancel(&t->ev, t->delay_timer);
        t->delay_timer = -1;
    }
}
static int co_arm_deadline(void *ctx, int *out_err)
{
    HlSmtpTransport *t = ctx;
    /* Section 8: the connect-deadline timer receives Dop - now, NEVER a fresh full
     * timeout, so connect shares the single post-resolution ceiling. */
    uint64_t now = kl_monotonic_ms();
    uint64_t delay = t->dop_ms > now ? t->dop_ms - now : 0;
    t->deadline_timer = kl_timer_add(&t->ev, delay, co_on_deadline_fired, t);
    if (t->deadline_timer < 0) { *out_err = (int)KL_ERR_TIMEOUT; return -1; }
    return 0;
}
static void co_cancel_deadline(void *ctx)
{
    HlSmtpTransport *t = ctx;
    if (t->deadline_timer >= 0) {
        kl_timer_cancel(&t->ev, t->deadline_timer);
        t->deadline_timer = -1;
    }
}

static void co_on_done(void *ctx, KlConnectResult result, KlSocketHandle fd, int error)
{
    HlSmtpTransport *t = ctx;
    t->connect_done = 1;
    t->connect_result = result;
    t->connect_error = error;
    if (result == KL_CONNECT_SUCCESS) {
        t->fd = fd;
        if (tp_stream_bringup(t) != 0) {
            /* Bring-up failed: retire the fd and mark the op failed for the pump. */
            t->connect_result = KL_CONNECT_FAILED;
            if (kl_handle_valid(t->fd)) { sp_close(t->fd); t->fd = KL_INVALID_SOCKET; }
        }
    }
}

static void co_on_detach(void *ctx)
{
    HlSmtpTransport *t = ctx;
    t->connect_detached = 1;
}

static const KlConnectOpHooks SMTP_CONNECT_HOOKS = {
    .start_resolve   = co_start_resolve,
    .cancel_resolve  = NULL,
    .start_attempt   = co_start_attempt,
    .cancel_attempt  = co_cancel_attempt,
    .dispose_fd      = co_dispose_fd,
    .arm_delay       = co_arm_delay,
    .cancel_delay    = co_cancel_delay,
    .arm_deadline    = co_arm_deadline,
    .cancel_deadline = co_cancel_deadline,
    .on_done         = co_on_done,
    .on_detach       = co_on_detach,
};

/* ────────────────────────────────────────────────────────────────────────────
 * Event-loop pump. Runs the private KlEventCtx until @p done() or the per-stage
 * deadline elapses so a dead peer cannot hang the synchronous caller.
 *
 * The deadline is a MONOTONIC ABSOLUTE instant (kl_monotonic_ms() + budget),
 * NOT an accumulation of assumed tick durations: kl_event_ctx_run may return
 * early (readiness arrived) or late (a timer fired), so summing step_ms would
 * drift. Each kl_event_ctx_run waits up to step_ms for readiness; the signature
 * is (ctx, max_events, timeout_ms) - max_events 64, timeout step_ms.
 *
 * This bounds ONE stage, and each stage's deadline is itself clamped to the
 * frozen post-resolution operation ceiling Dop (section 8), so no stage or retry
 * can extend the operation - the total-operation deadline IS implemented (see
 * pump_check + hl_smtp_transport_connect). NOTE: getaddrinfo runs blocking on the
 * calling thread (see resolve_addrs); Dop is armed AFTER it returns, so DNS is
 * outside the ceiling per section 8 (a bounded sandbox-safe resolver is a separate
 * future item).
 * ────────────────────────────────────────────────────────────────────────── */

typedef int (*DonePred)(HlSmtpTransport *t);

#ifdef HL_SMTP_TEST_HOOKS
/* ── Test-only pump seam (compiled ONLY under -DHL_SMTP_TEST_HOOKS) ────────────
 * ABSENT from the production object. It lets unit tests drive the connect state
 * machine deterministically WITHOUT diverging from Keel's clock: every deadline
 * here still reads the real kl_monotonic_ms() (ONE clock domain), and the hook
 * only OBSERVES each pump checkpoint and/or ALIGNS two readiness conditions (Dop
 * expiry + cancellation) onto a single checkpoint. It never advances a private
 * clock (which would let a pump deadline expire while a Keel connect / Happy-
 * Eyeballs timer, still on real time, never fires - an impossible runtime state).
 *
 * When non-NULL, smtp_test_checkpoint fires at the TOP of every pump_check with
 * the transport and a monotonically-increasing checkpoint index, BEFORE the frozen
 * precedence is evaluated - so a test can record attempt/timestamp state or arm a
 * same-checkpoint deadline-vs-cancel race. */
void (*smtp_test_checkpoint)(HlSmtpTransport *t, unsigned idx);
static unsigned smtp_test_checkpoint_seq;
#endif

/* One classification pass at a pump check point, in the FROZEN precedence
 * (section 8):
 *   1. a completed predicate wins (deliver the stage result);
 *   2. THEN an expired Dop is classified (dop_expired set) - so a Dop-vs-cancel
 *      race tags terminal:post_resolution_deadline, never terminal:cancelled;
 *   3. THEN cancellation is honored (prompt abort of an in-flight conversation);
 *   4. THEN the stage-only budget.
 * Returns 1 = done (pump success), -1 = terminate, 0 = keep pumping. */
static int pump_check(HlSmtpTransport *t, DonePred done, uint64_t deadline_ms)
{
#ifdef HL_SMTP_TEST_HOOKS
    if (smtp_test_checkpoint)
        smtp_test_checkpoint(t, smtp_test_checkpoint_seq++);
#endif
    if (done(t))
        return 1;
    uint64_t now = kl_monotonic_ms();
    if (t->dop_ms && now >= t->dop_ms) { t->dop_expired = 1; return -1; }
    if (t->cancel_poll && t->cancel_poll(t->cancel_user))
        return -1;
    if (now >= deadline_ms)
        return -1;
    return 0;
}

/* Pump until done() or a terminate condition (Dop / cancel / stage deadline). The
 * check runs BEFORE and AFTER each event-loop step, so a condition that arrives
 * during the step takes effect within one step (~50 ms). */
static int pump_until_abs(HlSmtpTransport *t, DonePred done, uint64_t deadline_ms)
{
    const int step_ms = 50;
    for (;;) {
        int c = pump_check(t, done, deadline_ms);
        if (c) return c > 0 ? 0 : -1;
        if (kl_event_ctx_run(&t->ev, 64, step_ms) < 0)
            return -1;
        c = pump_check(t, done, deadline_ms);
        if (c) return c > 0 ? 0 : -1;
    }
}

/* One stage bounded by Dstage = min(Dop, now + stage_budget) - so no stage or
 * retry can extend the frozen post-resolution ceiling Dop (section 8). Dop is 0
 * only on the pre-resolution paths (none pump); a set Dop always wins the min. */
static int pump_until(HlSmtpTransport *t, DonePred done, int timeout_ms)
{
    int budget = timeout_ms > 0 ? timeout_ms : HL_SMTP_DEFAULT_TIMEOUT_MS;
    uint64_t stage = kl_monotonic_ms() + (uint64_t)budget;
    uint64_t dl = (t->dop_ms && t->dop_ms < stage) ? t->dop_ms : stage;
    return pump_until_abs(t, done, dl);
}

/* Predicates. */
static int done_connect(HlSmtpTransport *t)
{ return t->connect_done && (t->connect_result != KL_CONNECT_SUCCESS || t->stream_up); }
static int done_reply(HlSmtpTransport *t)
{
    /* A complete reply is available, OR the read side hit terminal EOF/error. */
    char peek[SMTP_MAX_REPLY];
    SmtpReplyAcc snapshot = t->acc;   /* non-destructive: copy then try-take */
    int code = reply_acc_take(&snapshot, peek, (int)sizeof peek);
    return code != 0 || t->read_eof;
}
static int done_write(HlSmtpTransport *t)
{ return kl_stream_write_pending(&t->stream) == 0 || t->write_error || t->read_eof; }
static int done_tls(HlSmtpTransport *t)      { return t->tls_done; }
static int done_detached(HlSmtpTransport *t) { return t->closed; }
static int done_connect_detached(HlSmtpTransport *t)
{ return kl_connect_op_is_detached(&t->connect_op); }

/* ────────────────────────────────────────────────────────────────────────────
 * Public API.
 * ────────────────────────────────────────────────────────────────────────── */

HlSmtpTransport *hl_smtp_transport_connect(const char *host, int port,
                                           int timeout_ms,
                                           int (*cancel_poll)(void *),
                                           void *cancel_user,
                                           int *out_teardown_leaked,
                                           int *out_dop_expired)
{
    if (out_teardown_leaked)
        *out_teardown_leaked = 0;
    if (out_dop_expired)
        *out_dop_expired = 0;
    if (!host || port < 1 || port > 65535)
        return NULL;
    (void)socket_provider();   /* one-time init of g_sp */

    HlSmtpTransport *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->alloc = kl_allocator_default();
    t->fd = KL_INVALID_SOCKET;
    t->deadline_timer = -1;
    t->delay_timer = -1;
    t->deadline_ms = (uint64_t)(timeout_ms > 0 ? timeout_ms : HL_SMTP_DEFAULT_TIMEOUT_MS);
    t->connect_result = KL_CONNECT_CANCELLED;
    for (int i = 0; i < KL_CONNECT_MAX_ADDRS; i++)
        t->attempt_fd[i] = KL_INVALID_SOCKET;
    reply_acc_reset(&t->acc);

    if (kl_event_ctx_init(&t->ev, &t->alloc) != 0) {
        free(t);
        return NULL;
    }
    t->ev_ready = 1;

    /* Sandbox-compatible blocking system resolve, inline before start. Blocking
     * getaddrinfo is the documented non-interruptible exception; cancellation
     * takes effect from HERE on (the connect pump below is the first to check). */
    t->naddrs = resolve_addrs(t, host, port);
    t->cancel_poll = cancel_poll;
    t->cancel_user = cancel_user;

    /* Freeze the post-resolution operation deadline (Dop): one absolute ceiling
     * for connect + every subsequent stage/retry. Set HERE, after the blocking
     * (non-interruptible) resolve, so DNS is outside the ceiling per section 8. */
    t->dop_ms = kl_monotonic_ms() + t->deadline_ms;

    if (kl_connect_op_init(&t->connect_op, &SMTP_CONNECT_HOOKS, t) != 0) {
        hl_smtp_transport_free(t);
        return NULL;
    }
    if (kl_connect_op_start(&t->connect_op) != 0) {
        hl_smtp_transport_free(t);
        return NULL;
    }
    /* Fix 2: the op is now live; teardown must cancel it and confirm detachment
     * before releasing its embedded storage. */
    t->connect_started = 1;

    if (pump_until(t, done_connect, timeout_ms) != 0 ||
        t->connect_result != KL_CONNECT_SUCCESS || !t->stream_up) {
        /* Capture the connect-phase Dop expiry BEFORE free reclaims the transport,
         * so a connect that reached Dop still surfaces deadline_expired (else the
         * NULL return loses t->dop_expired) - mirrors out_teardown_leaked. */
        if (out_dop_expired && t->dop_expired)
            *out_dop_expired = 1;
        /* This is the most plausible non-detaching-op site (a connect that never
         * completed). Propagate the teardown outcome so hl_cap_smtp_send can
         * record "teardown":"leaked" in the audit rather than losing it. */
        if (hl_smtp_transport_free(t) != 0 && out_teardown_leaked)
            *out_teardown_leaked = 1;
        return NULL;
    }
    return t;
}

/* Fail-closed cleanup when a TLS handshake never completed (begin failed, or the
 * pump timed out mid-handshake). Fix 7: explicitly remove the dedicated
 * handshake watcher BEFORE destroying the KlTls and closing the fd, then abort
 * the stream. Fix 6: NO plaintext fallback / NO kl_stream_resume - the stream is
 * cancelled (abortive close), never resumed in plaintext. */
static void tp_tls_abort(HlSmtpTransport *t)
{
    if (t->hs_watch_armed) {
        kl_watcher_del(&t->ev, t->fd);
        t->hs_watch_armed = 0;
        t->stream_ws.armed = 0;
        t->stream_ws.mask  = 0;
    }
    if (t->tls) {
        t->tls->destroy(t->tls);
        t->tls = NULL;
    }
    t->tls_up = 0;
    t->tls_handshaking = 0;
    t->tls_failed = 1;
    kl_stream_cancel(&t->stream);   /* fail closed; no plaintext continuation */
}

int hl_smtp_transport_implicit_tls(HlSmtpTransport *t, const char *host,
                                   void *tls_cfg, int timeout_ms)
{
    if (!t || !t->stream_up || t->tls_up)
        return -1;
    /* Pause the raw read side; the handshake owns the fd (no app bytes yet). */
    kl_stream_pause(&t->stream);
    if (tp_tls_begin_handshake(t, host, tls_cfg) != 0) {
        tp_tls_abort(t);
        return -1;
    }
    if (pump_until(t, done_tls, timeout_ms) != 0 || t->tls_failed || !t->tls_up) {
        tp_tls_abort(t);
        return -1;
    }
    return 0;
}

int hl_smtp_transport_starttls(HlSmtpTransport *t, const char *host,
                               void *tls_cfg, int timeout_ms)
{
    if (!t || !t->stream_up || t->tls_up)
        return -1;
    /* Fix 6: the caller has consumed the STARTTLS 220 reply; require the
     * plaintext accumulator to be EMPTY. Any buffered bytes past the 220 are a
     * STARTTLS-injection attempt (a MITM prepending plaintext to the ciphertext
     * boundary) - abort fail-closed and do NOT upgrade. Do not rely on the
     * caller's stated precondition. */
    if (t->acc.len != 0 || t->acc.overflow) {
        kl_stream_cancel(&t->stream);
        t->tls_failed = 1;
        return -1;
    }
    /* INVARIANT 2: pause plaintext reads FIRST, before handing the fd to TLS,
     * so no plaintext read can consume ClientHello bytes. */
    kl_stream_pause(&t->stream);
    if (tp_tls_begin_handshake(t, host, tls_cfg) != 0) {
        tp_tls_abort(t);
        return -1;
    }
    if (pump_until(t, done_tls, timeout_ms) != 0 || t->tls_failed || !t->tls_up) {
        tp_tls_abort(t);
        return -1;
    }
    return 0;
}

int hl_smtp_transport_tls_active(const HlSmtpTransport *t)
{
    return t && t->tls_up ? 1 : 0;
}

int hl_smtp_transport_write(HlSmtpTransport *t, const void *data, size_t len,
                            int timeout_ms)
{
    if (!t || !t->stream_up)
        return -1;
    if (len == 0)
        return 0;

    /* Fix 1: BOUNDED CHUNKED ADMISSION with ordered drain. kl_stream_write is
     * atomic and the write queue is SMTP_WRITE_QUEUE_BYTES; a single >queue
     * write returns KL_STREAM_TOO_LARGE. So admit the payload in
     * SMTP_WRITE_CHUNK pieces, draining pending bytes between chunks so total
     * queued memory stays bounded (never the whole 10 MiB message). External
     * contract unchanged: 0 iff every byte is sent, -1 on error/timeout.
     *
     * The WHOLE write stage shares ONE absolute deadline. Every chunk drain +
     * WOULD_BLOCK retry pumps against the SAME `deadline`, so a slow peer cannot
     * stretch one 10 MiB DATA write across N x timeout_ms. And that deadline is
     * itself clamped to the frozen post-resolution ceiling Dop (section 8), so the
     * write stage - like every other stage - can never extend the operation. */
    int budget = timeout_ms > 0 ? timeout_ms : HL_SMTP_DEFAULT_TIMEOUT_MS;
    uint64_t stage = kl_monotonic_ms() + (uint64_t)budget;
    uint64_t deadline = (t->dop_ms && t->dop_ms < stage) ? t->dop_ms : stage;

    const char *p = (const char *)data;
    size_t remaining = len;
    while (remaining > 0) {
        /* Drain what is already queued before admitting the next chunk, so the
         * queue has room and bounded memory is held at any instant. A dead peer
         * trips the shared write-stage deadline. */
        if (kl_stream_write_pending(&t->stream) > 0) {
            if (pump_until_abs(t, done_write, deadline) != 0)
                return -1;
            if (t->write_error || t->read_eof)
                return -1;
        }
        size_t chunk = remaining < SMTP_WRITE_CHUNK ? remaining : SMTP_WRITE_CHUNK;
        KlStreamWriteStatus st = kl_stream_write(&t->stream, p, chunk);
        if (st == KL_STREAM_WOULD_BLOCK) {
            /* No room right now: drain, then retry this same chunk. */
            if (pump_until_abs(t, done_write, deadline) != 0)
                return -1;
            if (t->write_error || t->read_eof)
                return -1;
            continue;
        }
        if (st != KL_STREAM_ACCEPTED)
            return -1;   /* TOO_LARGE (chunk <= queue, so never), CLOSED, ERROR */
        p += chunk;
        remaining -= chunk;
    }

    /* Final drain: every admitted byte must physically leave the queue, still
     * bounded by the one shared write-stage deadline. */
    if (pump_until_abs(t, done_write, deadline) != 0)
        return -1;
    if (t->write_error || kl_stream_write_pending(&t->stream) != 0)
        return -1;
    return 0;
}

int hl_smtp_transport_read_reply(HlSmtpTransport *t, char *buf, int size,
                                 int timeout_ms)
{
    if (!t || !t->stream_up || size <= 0)
        return -1;

    /* A prior read may have delivered a pipelined reply already; take first. */
    int code = reply_acc_take(&t->acc, buf, size);
    if (code > 0)
        return code;
    if (code < 0)
        return -1;

    if (pump_until(t, done_reply, timeout_ms) != 0)
        return -1;

    code = reply_acc_take(&t->acc, buf, size);
    if (code > 0)
        return code;
    return -1;   /* EOF / parse error / timeout with no complete reply */
}

int hl_smtp_transport_dop_expired(const HlSmtpTransport *t)
{
    return t ? t->dop_expired : 0;
}

void hl_smtp_transport_shutdown(HlSmtpTransport *t)
{
    if (!t || !t->stream_up || t->close_begun)
        return;
    /* Teardown must run to CONFIRMED detachment (bounded by the grace below), so a
     * still-pending cancel must NOT short-circuit these pumps - that would skip
     * detachment and leak the fd/stream on every cancelled shutdown. Disarm it. */
    t->cancel_poll = NULL;
    t->close_begun = 1;
    kl_stream_close_begin(&t->stream);
    /* Drive to confirmed detachment, but only for a SHORT grace, not the full
     * operation timeout: a graceful close drains queued output + sends
     * close_notify, which on a healthy peer completes almost immediately. If the
     * peer is gone or stalled with an undrained queue (e.g. after a failed
     * write), a graceful drain can never complete, so cap the wait and let
     * hl_smtp_transport_free's ABORTIVE cancel finish teardown fast rather than
     * blocking the synchronous caller for the whole operation timeout. */
    pump_until(t, done_detached, SMTP_CLOSE_GRACE_MS);
}

int hl_smtp_transport_free(HlSmtpTransport *t)
{
    if (!t)
        return 0;

    /* Abortive teardown must confirm detachment (fail-closed, no UAF), so its
     * pumps ignore a pending cancel AND the (possibly-exhausted) Dop - clear both
     * before any of them run, else an expired Dop would clip the confirm and leak
     * the fd/stream. The graceful close in _shutdown keeps its Dop bound (section
     * 8); this last-resort teardown does not. */
    t->cancel_poll = NULL;
    t->dop_ms = 0;

    /* If a stream was brought up but not gracefully closed, cancel it and pump
     * to confirmed detachment so every op/watcher/fd retires exactly once. Apply
     * the same fail-closed rule as the connect op below: if the stream does NOT
     * reach confirmed detachment (t->closed, via tp_stream_on_close) within the
     * bound, live stream recv/send callbacks may still reference this storage, so
     * we must NOT destroy TLS / close the fd / free the event ctx and `t`. Leak
     * the storage intentionally and report it, rather than free into a
     * use-after-free. */
    if (t->stream_up && !t->closed) {
        kl_stream_cancel(&t->stream);
        pump_until(t, done_detached, HL_SMTP_DEFAULT_TIMEOUT_MS);
        if (!t->closed) {
            log_error("smtp: stream did not detach within the bound; leaking "
                      "transport storage rather than freeing a live stream");
            return -1;
        }
    }

    /* Fix 2: the connect op owns any racing descriptors + the delay/deadline
     * timers. If it was started and has NOT yet reached confirmed detachment
     * (connect failed/cancelled mid-race, or the stream never came up), we may
     * NOT free its embedded storage or the event ctx until it detaches: cancel
     * it (retiring every racing fd via its cancel_attempt/dispose_fd hooks and
     * both timers via cancel_delay/cancel_deadline), then pump to detachment.
     * Fail LOUDLY if it will not detach within the bound rather than silently
     * freeing storage a live op still references. */
    if (t->connect_started && !kl_connect_op_is_detached(&t->connect_op)) {
        kl_connect_op_cancel(&t->connect_op);
        if (pump_until(t, done_connect_detached, HL_SMTP_DEFAULT_TIMEOUT_MS) != 0 ||
            !kl_connect_op_is_detached(&t->connect_op)) {
            log_error("smtp: connect op did not detach within the bound; "
                      "leaking transport storage rather than freeing a live op");
            /* Observable to the caller: storage is intentionally leaked (the op
             * still references it) rather than freed into a use-after-free. */
            return -1;
        }
    }

    /* on_close (tp_stream_on_close) frees TLS + fd. Belt-and-suspenders for
     * paths where the stream never came up (e.g. connect failure): retire any
     * TLS + fd + watchers directly. */
    if (t->tls) {
        t->tls->destroy(t->tls);
        t->tls = NULL;
        t->tls_up = 0;
    }
    if (kl_handle_valid(t->fd)) {
        stream_watch_clear(t);
        sp_close(t->fd);
        t->fd = KL_INVALID_SOCKET;
    }
    /* Last-resort fallback: the connect op's cancel path already retires racing
     * attempt fds via cancel_attempt/dispose_fd, so this normally finds none.
     * It closes any fd that somehow escaped (never-started op, a hook that could
     * not route ownership). */
    for (int i = 0; i < KL_CONNECT_MAX_ADDRS; i++) {
        if (kl_handle_valid(t->attempt_fd[i])) {
            if (t->conn_ws[i].armed)
                kl_watcher_del(&t->ev, t->attempt_fd[i]);
            sp_close(t->attempt_fd[i]);
            t->attempt_fd[i] = KL_INVALID_SOCKET;
        }
    }
    if (t->delay_timer >= 0) {
        kl_timer_cancel(&t->ev, t->delay_timer);
        t->delay_timer = -1;
    }
    if (t->deadline_timer >= 0) {
        kl_timer_cancel(&t->ev, t->deadline_timer);
        t->deadline_timer = -1;
    }
    /* Fix 8: free the write queue whenever it was initialized, independent of
     * whether the full stream bring-up completed (a partial bring-up leaves
     * write_q_inited=1, stream_up=0). */
    if (t->write_q_inited)
        kl_stream_write_free(&t->stream);
    if (t->ev_ready)
        kl_event_ctx_free(&t->ev);
    free(t);
    return 0;
}
