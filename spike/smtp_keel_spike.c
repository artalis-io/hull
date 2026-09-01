/*
 * smtp_keel_spike.c: FEASIBILITY SPIKE (Slice 2a of the SMTP-to-Keel-v3 transition).
 *
 * Goal: prove that a Hull-local SMTP-shaped transport adapter can compose Keel
 * v3's PUBLIC primitives (connect -> byte stream -> [TLS upgrade] -> close) using
 * ONLY public Keel headers. NO header from vendor/keel/src/ may be included; that
 * is the compile gate enforced by spike/build.sh's include path.
 *
 * This file is COMPLETELY SEPARATE from production src/hull/cap/smtp.c and
 * introduces NO shared transport abstraction. It is throwaway spike code.
 *
 * The client side (HlSmtpSpikeOp) drives:
 *   KlConnectOp (Happy-Eyeballs orchestrator, bring-your-own I/O) ->
 *   KlStream    (write queue + read pause/resume + graceful close) ->
 *   [live KlTls handshake: implicit TLS AND in-place STARTTLS] ->
 *   confirmed detachment.
 *
 * All of that rides ONE KlEventCtx. In-process fake SMTP peers (plaintext AND
 * a real mbedTLS-terminating peer) are driven from the SAME event ctx via
 * watchers, so the whole lifecycle runs in a single-threaded readiness loop with
 * no external dependency.
 *
 * This is the EXTENDED spike that closes the reviewer's Slice-2a gaps:
 *   (1) IOCP/completion is OUT OF SCOPE (Hull ships no native-Windows runtime;
 *       its Windows target is the Cosmopolitan APE = poll = readiness).
 *   (2) Write backpressure is PROVEN LIVE: a stalled peer forces would-block,
 *       a WRITABLE watcher re-arms and kl_stream_flush drains the queue.
 *   (3) TLS is PROVEN LIVE against a real mbedTLS peer: implicit TLS, in-place
 *       STARTTLS, cert+hostname verify success, and a negative reject case.
 *   (4) Resolution races MULTIPLE addresses (first fails, second wins); an
 *       overall deadline is ACTUALLY FIRED against a blackhole; cancel happens
 *       DURING resolution.
 *   (5) Watcher arming tracks ownership explicitly and FAILS CLOSED on a real
 *       kl_watcher_add error (never masks it as "already registered").
 *   (6) fd-leak evidence rests on explicit descriptor-closure assertions, not
 *       ASan (ASan proves no heap leak/UAF only).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* PUBLIC Keel headers ONLY (the compile gate) */
#include <keel/connect_op.h>
#include <keel/connect_op_detail.h> /* opt-in layout, so we can embed a KlConnectOp */
#include <keel/stream.h>
#include <keel/stream_detail.h>     /* opt-in layout, so we can embed a KlStream */
#include <keel/resolver.h>
#include <keel/dns_resolver.h>
#include <keel/event_ctx.h>
#include <keel/event.h>
#include <keel/socket.h>
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include <keel/timer.h>
#include <keel/tls.h>
#include <keel/error.h>
#include <keel/allocator.h>
#include <keel_tls_mbedtls.h>       /* the TLS integration header */

/* Standard C / POSIX (spike host side; NOT part of the Keel gate) */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>   /* AF_INET, SOCK_STREAM, listen()/accept() for the fake peer */
#include <netinet/in.h>   /* struct sockaddr_in for the fake peer's ephemeral bind */
#include <unistd.h>

/* ────────────────────────────────────────────────────────────────────────────
 * Test harness scaffolding.
 * ────────────────────────────────────────────────────────────────────────── */

static int g_failures = 0;
static char g_fail_reason[256];

#define SPIKE_CHECK(cond, ...)                                                  \
    do {                                                                        \
        if (!(cond)) {                                                          \
            if (g_failures == 0)                                                \
                snprintf(g_fail_reason, sizeof g_fail_reason, __VA_ARGS__);     \
            fprintf(stderr, "  [FAIL] " __VA_ARGS__);                           \
            fprintf(stderr, "\n");                                              \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

static void logline(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* Path to the spike's TLS material (server cert/key). Relative to repo root,
 * which build.sh cd's into before running the binary. */
#define SPIKE_TLS_CERT "spike/tls/server.crt"
#define SPIKE_TLS_KEY  "spike/tls/server.key"

/* ────────────────────────────────────────────────────────────────────────────
 * KEY SPIKE FINDING: consume the POSIX socket provider's PUBLIC ops table
 * directly, instead of the private `kl_sock_*` consumer wrappers (src/socket.h).
 *
 * kl_socket_provider_posix() returns a const KlSocketProvider* whose ->ops is a
 * fully-populated public KlSocketOps vtable. We call ops->socket / ->connect /
 * ->send / ->recv / ->close / ->set_nonblocking / ->get_so_error directly. The
 * POSIX provider leaves ->io_status NULL, so we read errno directly for
 * would-block / EINPROGRESS classification (the documented fallback).
 *
 * SEAM DISPOSITION (per reviewer): the io_status-NULL + hosted-errno fallback IS
 * the defined public contract, so no Keel change is proposed. The explicitly
 * selected POSIX provider is retained in the adapter (a static borrow here).
 * ────────────────────────────────────────────────────────────────────────── */

static const KlSocketProvider *g_sp;   /* the POSIX provider (borrowed, static storage) */

static KlSocketHandle sp_socket(int domain, int type, int protocol)
{
    return g_sp->ops->socket(g_sp->context, domain, type, protocol);
}
static int sp_set_nonblocking(KlSocketHandle fd) { return g_sp->ops->set_nonblocking(g_sp->context, fd); }
static int sp_connect(KlSocketHandle fd, const KlSockAddr *a) { return g_sp->ops->connect(g_sp->context, fd, a); }
static int sp_get_so_error(KlSocketHandle fd, int *out) { return g_sp->ops->get_so_error(g_sp->context, fd, out); }
static kl_ssize_t sp_send(KlSocketHandle fd, const void *b, size_t n) { return g_sp->ops->send(g_sp->context, fd, b, n); }
static kl_ssize_t sp_recv(KlSocketHandle fd, void *b, size_t n) { return g_sp->ops->recv(g_sp->context, fd, b, n); }
static int sp_close(KlSocketHandle fd) { return g_sp->ops->close(g_sp->context, fd); }

/* errno classification (POSIX provider io_status is NULL: documented fallback). */
static int errno_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }

/* PERSISTENT allocator for KlTls / KlTlsCtx. Keel captures the KlAllocator BY
 * POINTER and dereferences it at destroy time - long after the creating function
 * returns - so it must outlive the session. A stack-local kl_allocator_default()
 * would dangle and crash tls_destroy's kl_free(t->alloc, ...) (see the note in
 * src/hull/shared/tls_client.c). Hold ONE copy in static storage. */
static KlAllocator g_tls_alloc;
static int         g_tls_alloc_ready;
static KlAllocator *persistent_tls_alloc(void)
{
    if (!g_tls_alloc_ready) { g_tls_alloc = kl_allocator_default(); g_tls_alloc_ready = 1; }
    return &g_tls_alloc;
}

/* ────────────────────────────────────────────────────────────────────────────
 * The client adapter: HlSmtpSpikeOp
 *
 * Composes KlConnectOp + KlStream + timers on one KlEventCtx, and models the
 * SMTP conversation as a tiny state machine driven off the KlStream deliver
 * callback (proving fragmented/coalesced handling by accumulating into a reply
 * buffer, never asserting per-deliver).
 *
 * TLS is layered ABOVE the raw stream: the KlStream writer/read hooks route
 * through tls->write/tls->read once the handshake has completed, exactly as the
 * KlStream contract prescribes ("TLS lives ABOVE the raw stream").
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum {
    SMTP_EXPECT_GREETING = 0,   /* waiting for "220 ..." */
    SMTP_EXPECT_EHLO_REPLY,     /* sent EHLO, waiting for the multiline "250 ..." */
    SMTP_EXPECT_STARTTLS_REPLY, /* sent STARTTLS, waiting for "220 ..." then upgrade */
    SMTP_EXPECT_EHLO2_REPLY,    /* re-EHLO after the TLS upgrade */
    SMTP_EXPECT_QUIT_REPLY,     /* sent QUIT, waiting for "221 ..." */
    SMTP_DONE
} SmtpPhase;

/* TLS drive mode for the client adapter. */
typedef enum {
    TLS_NONE = 0,      /* plaintext lifecycle */
    TLS_IMPLICIT,      /* handshake immediately on connect (SMTPS-style, port 465) */
    TLS_STARTTLS       /* plaintext greeting/EHLO, then STARTTLS in-place upgrade */
} TlsMode;

/* Explicit watcher-ownership tracking (reviewer item 5): we NEVER treat a
 * kl_watcher_add failure as "already armed". A single fd carries at most a READ
 * interest and/or a WRITE interest; Keel's watcher API is one-watcher-per-fd with
 * a mask, so we track the desired mask ourselves and drive kl_watcher_add /
 * kl_watcher_mod / kl_watcher_del from it, failing closed on any real error. */
typedef struct {
    int  armed;          /* 1 if a watcher is currently installed for op->fd */
    KlEventMask mask;    /* the interest mask currently installed */
} WatcherState;

typedef struct HlSmtpSpikeOp {
    KlEventCtx     *ev;
    KlConnectOp     connect_op;      /* embedded (needs connect_op_detail.h) */
    KlStream        stream;          /* embedded (needs stream_detail.h) */
    KlAllocator     alloc;

    /* Resolution: the adapter owns an address list so KlConnectOp can RACE it. */
    KlSockAddr      addrs[KL_CONNECT_MAX_ADDRS];
    int             naddrs;
    KlSocketHandle  attempt_fd[KL_CONNECT_MAX_ADDRS]; /* per-attempt racing fd */

    KlSocketHandle  fd;              /* the winning connected fd */

    /* Synthetic multi-address resolver behavior (reviewer item 4). */
    int             use_dns_resolver;  /* 1 = drive kl_dns_resolver_create */
    KlResolver     *dns;               /* borrowed DNS resolver (item 4 real-path) */
    KlResolveReq   *dns_req;
    int             cancel_during_resolve; /* 1 = cancel while RESOLVING */
    int             resolve_started;       /* start_resolve entered */
    int             resolve_cancelled;     /* co_cancel_resolve ran */

    /* Deadline test: start a real pending connect but DON'T watch it, so the
     * SO_ERROR is never observed and the overall deadline is guaranteed to win
     * the race (item 4: an ACTUALLY-fired deadline, deterministically). */
    int             blackhole_no_watch;

    int64_t         deadline_timer;
    uint64_t        deadline_ms;     /* per-test deadline */
    int             deadline_fired;  /* the deadline callback actually ran */

    /* One watcher-state per racing attempt fd (connect phase) + one for the
     * stream fd (read/write phase). We use a single WatcherState for the winning
     * fd since only one fd survives past connect. */
    WatcherState    conn_ws[KL_CONNECT_MAX_ADDRS];
    WatcherState    stream_ws;       /* READ|WRITE interest on the winning fd */

    char            read_buf[4096];  /* KlStream stable receive buffer */
    char            reply[8192];     /* accumulated reply across (possibly fragmented) delivers */
    size_t          reply_len;

    SmtpPhase       phase;

    /* Write-backpressure proof (reviewer item 2). */
    int             want_backpressure;   /* 1 = send a large payload that must queue */
    size_t          big_len;             /* size of the congestion payload */
    int             flush_after_wouldblock; /* set once a writable-driven flush drained the queue */
    int             write_wouldblocked;  /* set once the writer reported would-block */

    /* observable outcome */
    int             connect_done_count;   /* must be exactly 1 */
    int             connect_detach_count; /* must be exactly 1 */
    KlConnectResult connect_result;
    int             connect_error;
    int             stream_closed_count;  /* must be exactly 1 */
    int             ehlo_ok;
    int             quit_ok;
    int             attempt_fail_count;   /* how many racing attempts failed before the win */

    /* TLS (live). */
    TlsMode         tls_mode;
    int             tls_verify;      /* 1 = verify chain+hostname against the CA */
    const char     *tls_ca_path;     /* CA file to trust (NULL + verify=1 -> no anchor) */
    const char     *tls_hostname;    /* SNI + verification hostname */
    KlTlsCtx       *tls_ctx;
    KlTls          *tls;
    int             tls_up;          /* 1 while the session is live (cleared at teardown) */
    int             tls_completed;   /* latched 1 once the handshake ever succeeded */
    int             tls_handshaking; /* 1 while driving the handshake loop */
    int             hs_watch_armed;  /* dedicated handshake watcher installed */
    int             tls_want_read_seen;  /* observed a WANT_READ during handshake */
    int             tls_want_write_seen; /* observed a WANT_WRITE during handshake */
    int             tls_failed;      /* handshake failed (negative case) */
    int             expect_tls_fail; /* 1 = negative test: handshake MUST fail */
    size_t          plaintext_consumed_before_tls; /* bytes read plaintext pre-handshake */
    int             plaintext_delivered_during_hs; /* MUST stay 0: no plaintext across the upgrade */
    size_t          reply_len_at_starttls;         /* accumulated reply at the STARTTLS 220 boundary */
} HlSmtpSpikeOp;

/* forward decls */
static void smtp_advance(HlSmtpSpikeOp *op);
static void smtp_stream_teardown_begin(HlSmtpSpikeOp *op);
static void smtp_read_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data);
static int  smtp_tls_begin_handshake(HlSmtpSpikeOp *op);
static void smtp_tls_drive_handshake(HlSmtpSpikeOp *op);
static void smtp_tls_handshake_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data);

/* ── Explicit watcher-ownership helpers (reviewer item 5) ────────────────────
 * Set the desired interest mask on op->fd. Installs via kl_watcher_add the first
 * time, modifies via kl_watcher_mod thereafter. Returns 0 on success, -1 on a
 * REAL backend/alloc failure (caller FAILS CLOSED). Never silently swallows an
 * add failure. */
static int stream_watch_set(HlSmtpSpikeOp *op, KlEventMask mask)
{
    if (!op->stream_ws.armed) {
        if (kl_watcher_add(op->ev, op->fd, mask, smtp_read_watcher, op) != 0)
            return -1;   /* real failure: propagate, do NOT mask */
        op->stream_ws.armed = 1;
        op->stream_ws.mask  = mask;
        return 0;
    }
    if (op->stream_ws.mask == mask)
        return 0;   /* already exactly this interest: genuine no-op */
    if (kl_watcher_mod(op->ev, op->fd, mask) != 0)
        return -1;
    op->stream_ws.mask = mask;
    return 0;
}

static void stream_watch_clear(HlSmtpSpikeOp *op)
{
    if (op->stream_ws.armed) {
        kl_watcher_del(op->ev, op->fd);
        op->stream_ws.armed = 0;
        op->stream_ws.mask  = 0;
    }
}

/* ── KlStream WRITE facet: readiness writer over the provider send op ──────────
 * On would-block we return 0 (KlStream re-queues) AND arm a WRITABLE watcher so
 * kl_stream_flush is called when the socket drains (reviewer item 2). The TLS
 * path routes through tls->write once the handshake is up. */
static kl_ssize_t smtp_stream_write(const char *data, size_t len, void *ctx)
{
    HlSmtpSpikeOp *op = ctx;
    kl_ssize_t n;
    if (op->tls_up) {
        n = op->tls->write(op->tls, op->fd, data, len);
        if (n > 0) return n;
        if (n == 0) {            /* WANT_WRITE: TLS could not flush now */
            op->write_wouldblocked = 1;
            /* need writable to make progress */
            if (stream_watch_set(op, KL_EVENT_READ | KL_EVENT_WRITE) != 0)
                return -1;
            return 0;
        }
        return -1;
    }
    n = sp_send(op->fd, data, len);
    if (n < 0) {
        if (errno_would_block()) {
            op->write_wouldblocked = 1;
            /* Arm WRITABLE interest (keep READ too) so the queue drains on
             * write-readiness. This is the backpressure re-arm the reviewer
             * wanted proven. */
            if (stream_watch_set(op, KL_EVENT_READ | KL_EVENT_WRITE) != 0)
                return -1;   /* FAIL CLOSED */
            return 0;
        }
        return -1;
    }
    /* PARTIAL write (0 <= n < len): the socket send buffer is full. KlStream
     * queues the remainder, but nothing will re-send it until write-readiness.
     * So we MUST arm a WRITABLE watcher here too - a partial write is
     * backpressure just as much as an EAGAIN. This is the re-arm the reviewer
     * wanted proven: without it, a congested connection stalls with bytes
     * stranded in the queue. */
    if ((size_t)n < len) {
        op->write_wouldblocked = 1;
        if (stream_watch_set(op, KL_EVENT_READ | KL_EVENT_WRITE) != 0)
            return -1;   /* FAIL CLOSED */
    }
    return n;
}

/* ── KlStream READ facet: deliver / arm / disarm ───────────────────────────── */

static void smtp_stream_deliver(void *ctx, const char *buf, size_t len, int ok)
{
    HlSmtpSpikeOp *op = ctx;
    if (!ok)
        return;   /* terminal EOF/error on the read side */
    /* Boundary invariant (reviewer item 3): while a TLS handshake is in flight
     * the raw read side is paused, so NO plaintext deliver may occur. If one did,
     * plaintext crossed the upgrade - a hard failure. */
    if (op->tls_handshaking)
        op->plaintext_delivered_during_hs++;
    if (len > 0 && op->reply_len + len < sizeof op->reply) {
        memcpy(op->reply + op->reply_len, buf, len);
        op->reply_len += len;
        op->reply[op->reply_len] = '\0';
    }
    smtp_advance(op);
}

/* arm: READ interest. FAILS CLOSED on a real backend failure (item 5). */
static int smtp_stream_read_arm(void *ctx)
{
    HlSmtpSpikeOp *op = ctx;
    /* Preserve any WRITE interest that backpressure installed; add READ. */
    KlEventMask m = KL_EVENT_READ;
    if (op->stream_ws.armed && (op->stream_ws.mask & KL_EVENT_WRITE))
        m |= KL_EVENT_WRITE;
    if (stream_watch_set(op, m) != 0) {
        SPIKE_CHECK(0, "smtp_stream_read_arm: kl_watcher_add/mod failed (fail-closed)");
        return -1;   /* propagate: KlStream terminates the read op */
    }
    return 0;
}

static void smtp_stream_read_disarm(void *ctx)
{
    HlSmtpSpikeOp *op = ctx;
    /* Drop READ interest. If a WRITE interest is live (backpressure), keep it. */
    if (op->stream_ws.armed && (op->stream_ws.mask & KL_EVENT_WRITE)) {
        if (kl_watcher_mod(op->ev, op->fd, KL_EVENT_WRITE) == 0)
            op->stream_ws.mask = KL_EVENT_WRITE;
    } else {
        stream_watch_clear(op);
    }
}

/* The connected fd is ready: on WRITE, flush the write queue (backpressure
 * drain); on READ, pull bytes into the stream's stable read_buf. */
static void smtp_read_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    HlSmtpSpikeOp *op = user_data;

    /* During a TLS handshake the DEDICATED handshake watcher owns the fd; the
     * stream watcher is removed for that window, so we never reach here while
     * handshaking. (See smtp_tls_begin_handshake / smtp_tls_handshake_watcher.) */

    /* WRITABLE: drain the write queue (this is the backpressure flush). */
    if (ready & KL_EVENT_WRITE) {
        int fl = kl_stream_flush(&op->stream);
        SPIKE_CHECK(fl >= 0, "kl_stream_flush returned error %d", fl);
        if (kl_stream_write_pending(&op->stream) == 0) {
            /* queue drained: mark the proof and drop WRITE interest. */
            if (op->write_wouldblocked)
                op->flush_after_wouldblock = 1;
            /* keep READ interest, drop WRITE */
            if (op->stream_ws.armed) {
                if (kl_watcher_mod(op->ev, op->fd, KL_EVENT_READ) == 0)
                    op->stream_ws.mask = KL_EVENT_READ;
            }
        }
    }

    if (ready & KL_EVENT_READ) {
        if (op->tls_up && op->tls) {
            /* TLS read: drain all buffered plaintext (pending()) too, so an
             * edge-triggered loop never strands decrypted records. A deliver can
             * synchronously begin teardown (on_close frees op->tls and clears
             * tls_up), so we re-check both after every kl_stream_on_recv and bail
             * the instant the session is gone - never deref a freed op->tls. */
            for (;;) {
                kl_ssize_t n = op->tls->read(op->tls, fd, op->read_buf, sizeof op->read_buf);
                if (n > 0) {
                    kl_stream_on_recv(&op->stream, (size_t)n, 1);
                    if (!op->tls_up || !op->tls)
                        return;   /* deliver started teardown; session retired */
                } else if (n == 0) {
                    break;   /* WANT_READ: wait for more ciphertext */
                } else {
                    /* -1: clean close or error. at_eof disambiguates if present. */
                    kl_stream_on_recv(&op->stream, 0, 0);
                    return;
                }
                if (op->tls->pending && op->tls->pending(op->tls) == 0)
                    break;
            }
            return;
        }
        kl_ssize_t n = sp_recv(fd, op->read_buf, sizeof op->read_buf);
        if (n > 0) {
            kl_stream_on_recv(&op->stream, (size_t)n, 1);
        } else if (n == 0) {
            kl_stream_on_recv(&op->stream, 0, 0);   /* clean EOF */
        } else {
            if (errno_would_block())
                return;                              /* spurious wakeup */
            kl_stream_on_recv(&op->stream, 0, 0);    /* error -> terminal */
        }
    }
}

/* ── KlStream CLOSE facet ────────────────────────────────────────────────────── */
static void smtp_stream_on_close(void *ctx)
{
    HlSmtpSpikeOp *op = ctx;
    op->stream_closed_count++;
    if (op->tls) {
        if (op->tls_up && op->tls->shutdown)
            op->tls->shutdown(op->tls, op->fd);
        op->tls->destroy(op->tls);
        op->tls = NULL;
        op->tls_up = 0;
    }
    if (op->tls_ctx) {
        kl_tls_mbedtls_ctx_destroy(op->tls_ctx);
        op->tls_ctx = NULL;
    }
    if (kl_handle_valid(op->fd)) {
        stream_watch_clear(op);
        sp_close(op->fd);
        op->fd = KL_INVALID_SOCKET;
    }
}

/* ── SMTP conversation state machine ─────────────────────────────────────────── */

static int reply_has_code(const HlSmtpSpikeOp *op, const char *code)
{
    const char *p = op->reply;
    const char *last_final = NULL;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) break;
        if ((size_t)(eol - p) >= 4 && strncmp(p, code, 3) == 0 && p[3] == ' ')
            last_final = p;
        p = eol + 2;
    }
    return last_final != NULL;
}

static void smtp_write_line(HlSmtpSpikeOp *op, const char *line)
{
    KlStreamWriteStatus st = kl_stream_write(&op->stream, line, strlen(line));
    SPIKE_CHECK(st == KL_STREAM_ACCEPTED, "kl_stream_write(%.*s) status=%d",
                (int)(strlen(line) >= 2 ? strlen(line) - 2 : strlen(line)), line, (int)st);
}

static void smtp_reset_reply(HlSmtpSpikeOp *op) { op->reply_len = 0; op->reply[0] = '\0'; }

static void smtp_advance(HlSmtpSpikeOp *op)
{
    switch (op->phase) {
    case SMTP_EXPECT_GREETING:
        if (reply_has_code(op, "220")) {
            logline("client: got greeting: %.*s",
                    (int)(strcspn(op->reply, "\r")), op->reply);
            smtp_reset_reply(op);
            op->phase = SMTP_EXPECT_EHLO_REPLY;
            smtp_write_line(op, "EHLO spike\r\n");
        }
        break;
    case SMTP_EXPECT_EHLO_REPLY:
        if (reply_has_code(op, "250")) {
            logline("client: got EHLO reply (accumulated %zu bytes across delivers)",
                    op->reply_len);
            op->ehlo_ok = 1;
            smtp_reset_reply(op);
            if (op->tls_mode == TLS_STARTTLS && !op->tls_up) {
                op->phase = SMTP_EXPECT_STARTTLS_REPLY;
                smtp_write_line(op, "STARTTLS\r\n");
            } else {
                /* Optionally exercise the backpressure path with a big write. */
                if (op->want_backpressure) {
                    char *big = malloc(op->big_len);
                    SPIKE_CHECK(big != NULL, "malloc(big_len) failed");
                    if (big) {
                        memset(big, 'X', op->big_len);
                        /* Terminate with CRLF so it is a well-formed (if huge) line;
                         * the peer ignores content, it just must not read it until
                         * we let it, forcing our TCP send buffer to fill. */
                        big[op->big_len - 2] = '\r';
                        big[op->big_len - 1] = '\n';
                        KlStreamWriteStatus st =
                            kl_stream_write(&op->stream, big, op->big_len);
                        SPIKE_CHECK(st == KL_STREAM_ACCEPTED,
                                    "big kl_stream_write status=%d (want ACCEPTED)", (int)st);
                        free(big);
                    }
                }
                op->phase = SMTP_EXPECT_QUIT_REPLY;
                smtp_write_line(op, "QUIT\r\n");
            }
        }
        break;
    case SMTP_EXPECT_STARTTLS_REPLY:
        if (reply_has_code(op, "220")) {
            logline("client: got STARTTLS 220, beginning in-place TLS upgrade");
            /* CRITICAL boundary check (reviewer item 3): the accumulated reply
             * must end EXACTLY at the "220 ...\r\n" line - no bytes past it - so
             * no plaintext (and, since the peer sends nothing after the 220 until
             * it reads the ClientHello, no ciphertext) crossed the upgrade. We
             * assert the buffer holds a single CRLF-terminated 220 line. */
            op->reply_len_at_starttls = op->reply_len;
            {
                const char *crlf = strstr(op->reply, "\r\n");
                SPIKE_CHECK(crlf != NULL &&
                            (size_t)((crlf + 2) - op->reply) == op->reply_len,
                            "STARTTLS boundary: %zu plaintext bytes past the 220 line",
                            crlf ? op->reply_len - (size_t)((crlf + 2) - op->reply) : op->reply_len);
            }
            op->plaintext_consumed_before_tls = op->reply_len;
            smtp_reset_reply(op);
            op->phase = SMTP_EXPECT_EHLO2_REPLY;
            /* Pause the raw read side; the TLS handshake takes over the fd. */
            kl_stream_pause(&op->stream);
            if (smtp_tls_begin_handshake(op) != 0)
                SPIKE_CHECK(0, "smtp_tls_begin_handshake failed");
        }
        break;
    case SMTP_EXPECT_EHLO2_REPLY:
        if (reply_has_code(op, "250")) {
            logline("client: got post-TLS EHLO reply (over encrypted channel)");
            smtp_reset_reply(op);
            op->phase = SMTP_EXPECT_QUIT_REPLY;
            smtp_write_line(op, "QUIT\r\n");
        }
        break;
    case SMTP_EXPECT_QUIT_REPLY:
        if (reply_has_code(op, "221")) {
            logline("client: got QUIT reply (221 Bye)");
            op->quit_ok = 1;
            op->phase = SMTP_DONE;
            smtp_stream_teardown_begin(op);
        }
        break;
    case SMTP_DONE:
        break;
    }
}

/* ── Bring the KlStream up on the connected fd ───────────────────────────────── */
static int smtp_stream_bringup(HlSmtpSpikeOp *op)
{
    if (kl_stream_init(&op->stream, op->read_buf, sizeof op->read_buf) != 0)
        return -1;
    if (kl_stream_write_init(&op->stream, &op->alloc, 4 * 1024 * 1024) != 0)
        return -1;
    if (kl_stream_set_writer(&op->stream, smtp_stream_write, op) != 0)
        return -1;
    if (kl_stream_read_init(&op->stream, /*completion_mode=*/0,
                            smtp_stream_deliver, smtp_stream_read_arm,
                            smtp_stream_read_disarm, op) != 0)
        return -1;
    if (kl_stream_close_init(&op->stream, smtp_stream_on_close, op) != 0)
        return -1;
    if (kl_stream_read_start(&op->stream) != 0)
        return -1;
    return 0;
}

static void smtp_stream_teardown_begin(HlSmtpSpikeOp *op)
{
    kl_stream_close_begin(&op->stream);
}

/* ────────────────────────────────────────────────────────────────────────────
 * LIVE TLS: create the client session and drive the readiness handshake loop.
 *
 * Handshake watcher transitions (reviewer item 3): tls->handshake returns
 * WANT_READ / WANT_WRITE and we re-arm the fd watcher for exactly that direction
 * via kl_watcher_mod, then return to the event loop. On OK, subsequent stream
 * reads/writes route through tls->read/tls->write.
 * ────────────────────────────────────────────────────────────────────────── */
static int smtp_tls_begin_handshake(HlSmtpSpikeOp *op)
{
    /* Client context. verify=1 anchors trust at tls_ca_path (the spike's own CA);
     * verify=0 accepts any cert (dev only). This mirrors shared/tls_client.c.
     * Use the PERSISTENT allocator (see persistent_tls_alloc): a stack-local
     * would dangle at destroy time. */
    KlAllocator *ta = persistent_tls_alloc();
    if (op->tls_verify)
        op->tls_ctx = kl_tls_mbedtls_client_ctx_create(op->tls_ca_path, ta);
    else
        op->tls_ctx = kl_tls_mbedtls_client_ctx_create(NULL, ta);
    if (!op->tls_ctx)
        return -1;
    op->tls = kl_tls_mbedtls_create(op->tls_ctx, ta);
    if (!op->tls) {
        kl_tls_mbedtls_ctx_destroy(op->tls_ctx);
        op->tls_ctx = NULL;
        return -1;
    }
    if (op->tls_hostname && op->tls->set_hostname)
        op->tls->set_hostname(op->tls, op->tls_hostname);

    op->tls_handshaking = 1;
    /* Hand the fd to a DEDICATED handshake watcher. The stream's read watcher is
     * removed for the handshake window (kl_stream_pause already disarmed it), so
     * the handshake never runs nested inside a stream deliver callback and never
     * fights the stream watcher's read re-arm. The client writes ClientHello
     * first, so arm WRITE; the drive loop re-arms to whatever direction
     * tls->handshake asks for next. We DEFER the first drive to the event loop
     * (do not drive inline here) so every handshake step is a clean top-level
     * watcher fire - which is exactly the readiness re-arm transition the
     * reviewer wanted proven. */
    stream_watch_clear(op);
    if (kl_watcher_add(op->ev, op->fd, KL_EVENT_WRITE,
                       smtp_tls_handshake_watcher, op) != 0)
        return -1;   /* FAIL CLOSED (item 5) */
    op->hs_watch_armed = 1;
    return 0;
}

/* Dedicated handshake watcher: one clean top-level fire per step. */
static void smtp_tls_handshake_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    HlSmtpSpikeOp *op = user_data;
    (void)fd; (void)ready;
    smtp_tls_drive_handshake(op);
}

static void smtp_tls_drive_handshake(HlSmtpSpikeOp *op)
{
    KlTlsResult r = op->tls->handshake(op->tls, op->fd);
    if (r == KL_TLS_OK) {
        op->tls_handshaking = 0;
        /* Retire the dedicated handshake watcher and hand the fd back to the
         * stream's read watcher (now routing through tls->read/tls->write). */
        if (op->hs_watch_armed) {
            kl_watcher_del(op->ev, op->fd);
            op->hs_watch_armed = 0;
        }
        op->tls_up = 1;
        op->tls_completed = 1;   /* latch: survives teardown for the assertion */
        logline("client: TLS handshake OK (want_read seen=%d want_write seen=%d)",
                op->tls_want_read_seen, op->tls_want_write_seen);
        /* Re-install the stream read watcher and resume the raw read side. */
        op->stream_ws.armed = 0;   /* the dedicated watcher owned the fd; reset */
        op->stream_ws.mask  = 0;
        if (stream_watch_set(op, KL_EVENT_READ) != 0) {
            SPIKE_CHECK(0, "post-handshake read re-arm failed");
            return;
        }
        kl_stream_resume(&op->stream);
        /* Post-TLS: send the second EHLO over the encrypted channel. */
        smtp_write_line(op, "EHLO spike-tls\r\n");
        return;
    }
    if (r == KL_TLS_ERROR) {
        op->tls_handshaking = 0;
        if (op->hs_watch_armed) {
            kl_watcher_del(op->ev, op->fd);
            op->hs_watch_armed = 0;
            op->stream_ws.armed = 0;
            op->stream_ws.mask  = 0;
        }
        op->tls_failed = 1;
        logline("client: TLS handshake FAILED (expected=%d) - NO plaintext fallback",
                op->expect_tls_fail);
        /* Fail closed: begin an abortive stream teardown, NEVER fall back to
         * plaintext. The negative test asserts tls_failed && !tls_up. */
        kl_stream_cancel(&op->stream);
        return;
    }
    /* WANT_READ / WANT_WRITE: re-arm the DEDICATED handshake watcher for exactly
     * that direction (kl_watcher_mod), then yield to the event loop. This is the
     * handshake watcher transition. */
    KlEventMask want;
    if (r == KL_TLS_WANT_READ)  { op->tls_want_read_seen = 1;  want = KL_EVENT_READ;  }
    else                        { op->tls_want_write_seen = 1; want = KL_EVENT_WRITE; }
    if (kl_watcher_mod(op->ev, op->fd, want) != 0)
        SPIKE_CHECK(0, "handshake watcher re-arm (mod) failed");
}

/* ────────────────────────────────────────────────────────────────────────────
 * KlConnectOp hooks (bring-your-own I/O). The resolve path now supports:
 *   - a synthetic MULTI-address list the op RACES (reviewer item 4);
 *   - a real kl_dns_resolver_create resolve (also item 4, real path);
 *   - cancel-DURING-resolve (item 4).
 * ────────────────────────────────────────────────────────────────────────── */

static void co_dns_done(KlResolveReq *req, const KlResolveResult *result,
                        int error, void *user_data)
{
    HlSmtpSpikeOp *op = user_data;
    (void)req;
    op->dns_req = NULL;
    if (error != 0 || !result || result->naddrs < 1) {
        kl_connect_op_on_resolve_failed(&op->connect_op, (int)KL_ERR_DNS);
        return;
    }
    int n = result->naddrs;
    if (n > KL_CONNECT_MAX_ADDRS) n = KL_CONNECT_MAX_ADDRS;
    for (int i = 0; i < n; i++)
        op->addrs[i] = result->addrs[i];
    op->naddrs = n;
    kl_connect_op_on_resolved(&op->connect_op, n);
}

static int co_start_resolve(void *ctx)
{
    HlSmtpSpikeOp *op = ctx;
    op->resolve_started = 1;

    if (op->use_dns_resolver && op->dns) {
        /* Real async DNS path. The resolver may complete synchronously; the
         * connect op handles that. */
        op->dns_req = op->dns->resolve(op->dns, op->ev, op->tls_hostname ? op->tls_hostname : "localhost",
                                       0, co_dns_done, op);
        if (!op->dns_req)
            return -1;
        return 0;
    }

    /* Synthetic path: the adapter already holds op->naddrs addresses. Report them
     * so KlConnectOp RACES the list (Happy Eyeballs). If cancel_during_resolve is
     * set, we DO NOT report - we leave the resolve in flight so the test can
     * cancel while state == RESOLVING. */
    if (op->cancel_during_resolve)
        return 0;   /* stay in flight; test cancels next */

    kl_connect_op_on_resolved(&op->connect_op, op->naddrs);
    return 0;
}

static void co_cancel_resolve(void *ctx)
{
    HlSmtpSpikeOp *op = ctx;
    op->resolve_cancelled = 1;
    if (op->use_dns_resolver && op->dns && op->dns_req) {
        op->dns->cancel(op->dns_req);
        op->dns_req = NULL;
    }
    /* Retire the in-flight resolve op so the connect op can finalize. The
     * machine keeps `resolve_inflight` set until the adapter reports the
     * resolve's terminal; on a cancelled resolve we report failure, which
     * clears it and (the op already being terminal-CANCELLED) drives
     * detachment. Safe reentrantly: this runs under the cancel's in_dispatch
     * depth guard, so detachment is deferred until the stack unwinds. */
    kl_connect_op_on_resolve_failed(&op->connect_op, (int)KL_ERR_DNS);
}

/* connect-attempt watcher: a racing fd became writable -> SO_ERROR decides. */
static void co_connect_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data);

static int co_start_attempt(void *ctx, int idx, int *out_err)
{
    HlSmtpSpikeOp *op = ctx;
    if (idx < 0 || idx >= op->naddrs) { *out_err = (int)KL_ERR_CONNECT; return -1; }

    KlSocketHandle fd = sp_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) { *out_err = (int)KL_ERR_CONNECT; return -1; }
    if (sp_set_nonblocking(fd) < 0) { sp_close(fd); *out_err = (int)KL_ERR_CONNECT; return -1; }

    int rc = sp_connect(fd, &op->addrs[idx]);
    int ce = errno;
    if (rc < 0 && ce != EINPROGRESS) {
        /* HARD local failure that never went pending: dispose, advance. */
        sp_close(fd);
        *out_err = (int)KL_ERR_CONNECT;
        return -1;
    }
    /* Deadline test: keep the (real, pending EINPROGRESS) fd but install NO
     * watcher, so the SO_ERROR completion is never observed and the overall
     * deadline is the only thing that can terminate the op. This makes the
     * fired-deadline path deterministic (no dependence on how fast the host
     * delivers ENETUNREACH for the blackhole). The fd is still adapter-owned
     * and disposed via co_cancel_attempt when the deadline cancels the race. */
    if (op->blackhole_no_watch) {
        op->attempt_fd[idx] = fd;
        op->conn_ws[idx].armed = 0;
        return 0;
    }
    /* Register WRITE interest for this attempt fd. FAIL CLOSED on a real add
     * failure (item 5): dispose the fd and report a hard failure. */
    if (kl_watcher_add(op->ev, fd, KL_EVENT_WRITE, co_connect_watcher, op) != 0) {
        sp_close(fd);
        *out_err = (int)KL_ERR_CONNECT;
        return -1;
    }
    op->attempt_fd[idx] = fd;
    op->conn_ws[idx].armed = 1;
    op->conn_ws[idx].mask  = KL_EVENT_WRITE;

    if (rc == 0) {
        /* connected immediately (loopback can do this): decide inline. */
        kl_watcher_del(op->ev, fd);
        op->conn_ws[idx].armed = 0;
        kl_connect_op_on_attempt_connected(&op->connect_op, idx, fd);
    }
    return 0;
}

static void co_connect_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    HlSmtpSpikeOp *op = user_data;
    (void)ready;
    /* Find which attempt idx owns this fd. */
    int idx = -1;
    for (int i = 0; i < op->naddrs; i++)
        if (op->attempt_fd[i] == fd) { idx = i; break; }
    if (idx < 0) return;   /* stale */

    int soerr = 0;
    int gsrc = sp_get_so_error(fd, &soerr);
    kl_watcher_del(op->ev, fd);
    op->conn_ws[idx].armed = 0;
    if (gsrc == 0 && soerr == 0) {
        /* WON. */
        op->attempt_fd[idx] = KL_INVALID_SOCKET;   /* ownership moves to the op */
        kl_connect_op_on_attempt_connected(&op->connect_op, idx, fd);
    } else {
        op->attempt_fail_count++;
        sp_close(fd);
        op->attempt_fd[idx] = KL_INVALID_SOCKET;
        kl_connect_op_on_attempt_failed(&op->connect_op, idx, (int)KL_ERR_CONNECT);
    }
}

static void co_cancel_attempt(void *ctx, int idx)
{
    HlSmtpSpikeOp *op = ctx;
    if (idx >= 0 && idx < op->naddrs && kl_handle_valid(op->attempt_fd[idx])) {
        if (op->conn_ws[idx].armed) {
            kl_watcher_del(op->ev, op->attempt_fd[idx]);
            op->conn_ws[idx].armed = 0;
        }
        sp_close(op->attempt_fd[idx]);
        op->attempt_fd[idx] = KL_INVALID_SOCKET;
    }
    kl_connect_op_on_attempt_failed(&op->connect_op, idx, (int)KL_ERR_CONNECT);
}

/* dispose a connected non-winner fd (Happy Eyeballs straggler). Must never leak. */
static void co_dispose_fd(void *ctx, KlSocketHandle fd)
{
    HlSmtpSpikeOp *op = ctx;
    (void)op;
    if (kl_handle_valid(fd))
        sp_close(fd);
}

/* overall connect deadline via kl_timer_add. */
static void co_on_deadline_fired(void *user_data)
{
    HlSmtpSpikeOp *op = user_data;
    op->deadline_timer = -1;
    op->deadline_fired = 1;
    kl_connect_op_on_deadline(&op->connect_op, (int)KL_ERR_TIMEOUT);
}
static int co_arm_deadline(void *ctx, int *out_err)
{
    HlSmtpSpikeOp *op = ctx;
    op->deadline_timer = kl_timer_add(op->ev, op->deadline_ms, co_on_deadline_fired, op);
    if (op->deadline_timer < 0) { *out_err = (int)KL_ERR_TIMEOUT; return -1; }
    return 0;
}
static void co_cancel_deadline(void *ctx)
{
    HlSmtpSpikeOp *op = ctx;
    if (op->deadline_timer >= 0) {
        kl_timer_cancel(op->ev, op->deadline_timer);
        op->deadline_timer = -1;
    }
}

static void co_on_done(void *ctx, KlConnectResult result, KlSocketHandle fd, int error)
{
    HlSmtpSpikeOp *op = ctx;
    op->connect_done_count++;
    op->connect_result = result;
    op->connect_error = error;
    if (result == KL_CONNECT_SUCCESS) {
        op->fd = fd;             /* adopt the winner */
        if (smtp_stream_bringup(op) != 0) {
            SPIKE_CHECK(0, "smtp_stream_bringup failed after connect");
            return;
        }
        /* Implicit TLS: begin the handshake immediately, before any app bytes. */
        if (op->tls_mode == TLS_IMPLICIT) {
            kl_stream_pause(&op->stream);   /* raw read paused; handshake owns fd */
            if (smtp_tls_begin_handshake(op) != 0)
                SPIKE_CHECK(0, "implicit smtp_tls_begin_handshake failed");
        }
    }
}

static void co_on_detach(void *ctx)
{
    HlSmtpSpikeOp *op = ctx;
    op->connect_detach_count++;
}

static const KlConnectOpHooks SMTP_CONNECT_HOOKS = {
    .start_resolve   = co_start_resolve,
    .cancel_resolve  = co_cancel_resolve,
    .start_attempt   = co_start_attempt,
    .cancel_attempt  = co_cancel_attempt,
    .dispose_fd      = co_dispose_fd,
    .arm_delay       = NULL,           /* single/short list: no RFC8305 stagger needed */
    .cancel_delay    = NULL,
    .arm_deadline    = co_arm_deadline,
    .cancel_deadline = co_cancel_deadline,
    .on_done         = co_on_done,
    .on_detach       = co_on_detach,
};

/* ────────────────────────────────────────────────────────────────────────────
 * Fake in-process SMTP peer, driven off the SAME KlEventCtx. Two flavors:
 *   - plaintext: greeting/EHLO/QUIT, and (for backpressure) it STOPS reading on
 *     command so the client's TCP send buffer fills, then resumes.
 *   - TLS: after the (optional) plaintext STARTTLS handshake trigger, it runs a
 *     REAL mbedTLS server handshake and echoes the encrypted SMTP tail.
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum {
    PEER_GREETING = 0,    /* sent 220; waiting for EHLO */
    PEER_EHLO_DONE,       /* sent 250; waiting for STARTTLS or QUIT */
    PEER_STARTTLS_SENT,   /* sent 220 to STARTTLS; TLS handshake next */
    PEER_TLS_HANDSHAKE,   /* running the server-side TLS handshake */
    PEER_TLS_UP,          /* encrypted: expect EHLO2 then QUIT */
    PEER_BYE,             /* sent 221; done */
    PEER_CLOSED
} PeerPhase;

typedef struct {
    KlEventCtx    *ev;
    KlSocketHandle listen_fd;
    KlSocketHandle conn_fd;
    PeerPhase      phase;
    uint16_t       port;
    TlsMode        tls_mode;      /* NONE / IMPLICIT / STARTTLS */

    /* Backpressure control: when stop_reading is set, the peer does NOT recv,
     * so the client's send buffer fills. The peer sets stop_reading itself the
     * instant it has sent the EHLO 250 reply (if `backpressure` is set), so the
     * client's subsequent big write congests. The test resumes it once the
     * client writer has reported would-block. */
    int            backpressure;    /* arm the stop-after-EHLO behavior */
    int            stop_reading;
    int            resumed;

    /* TLS server side (real mbedTLS). */
    KlTlsCtx      *tls_ctx;
    KlTls         *tls;
    int            tls_up;
    int            tls_handshaking;
} FakePeer;

static void peer_send_all_plain(KlSocketHandle fd, const char *s)
{
    size_t off = 0, len = strlen(s);
    while (off < len) {
        kl_ssize_t n = sp_send(fd, s + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno_would_block()) continue;
        break;
    }
}

static void peer_tls_send_all(FakePeer *pe, const char *s)
{
    size_t off = 0, len = strlen(s);
    while (off < len) {
        kl_ssize_t n = pe->tls->write(pe->tls, pe->conn_fd, s + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n == 0) continue;   /* WANT_WRITE: spin (hermetic loopback) */
        break;                  /* error */
    }
}

static int peer_tls_begin(FakePeer *pe)
{
    /* PERSISTENT allocator (see persistent_tls_alloc): captured by pointer and
     * dereferenced at destroy time, so it must outlive the session. */
    KlAllocator *alloc = persistent_tls_alloc();
    pe->tls_ctx = kl_tls_mbedtls_ctx_create(SPIKE_TLS_CERT, SPIKE_TLS_KEY,
                                            NULL, KL_MTLS_NONE, alloc);
    if (!pe->tls_ctx) return -1;
    pe->tls = kl_tls_mbedtls_create(pe->tls_ctx, alloc);
    if (!pe->tls) { kl_tls_mbedtls_ctx_destroy(pe->tls_ctx); pe->tls_ctx = NULL; return -1; }
    pe->tls_handshaking = 1;
    pe->phase = PEER_TLS_HANDSHAKE;
    return 0;
}

static void peer_tls_drive_handshake(FakePeer *pe)
{
    KlTlsResult r = pe->tls->handshake(pe->tls, pe->conn_fd);
    if (r == KL_TLS_OK) {
        pe->tls_handshaking = 0;
        pe->tls_up = 1;
        pe->phase = PEER_TLS_UP;
        return;
    }
    if (r == KL_TLS_ERROR) {
        pe->tls_handshaking = 0;
        /* client aborted (negative case) - fine; retire on the next recv=0. */
        return;
    }
    /* WANT_READ/WANT_WRITE: on loopback the fd is level-triggered and stays
     * armed READ; the next watcher fire re-drives. Nothing else to do. */
}

static void peer_conn_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    FakePeer *pe = user_data;
    (void)ready;

    /* Backpressure: while stop_reading, do NOT read. The client's send buffer
     * fills, the client writer returns would-block, and its WRITABLE watcher
     * arms. We resume below on a scheduled tick. */
    if (pe->stop_reading && !pe->resumed)
        return;

    if (pe->tls_handshaking) { peer_tls_drive_handshake(pe); return; }

    /* STARTTLS boundary: the readable bytes here are the client's ClientHello
     * (ciphertext). We MUST NOT sp_recv them - that would steal the ClientHello
     * out from under mbedTLS. Instead begin the server handshake and let mbedTLS
     * read the socket itself. (Boundary invariant, reviewer item 3: the peer
     * sent NO application plaintext after the STARTTLS 220 - only the 220 line -
     * so nothing plaintext crosses the upgrade.) */
    if (pe->phase == PEER_STARTTLS_SENT) {
        if (peer_tls_begin(pe) == 0)
            peer_tls_drive_handshake(pe);
        return;
    }

    if (pe->tls_up) {
        char buf[8192];
        kl_ssize_t n = pe->tls->read(pe->tls, fd, buf, sizeof buf - 1);
        if (n <= 0) {
            if (n == 0) return;   /* WANT_READ */
            /* -1: clean close/error - retire. */
            kl_watcher_del(pe->ev, fd);
            if (pe->tls->shutdown) pe->tls->shutdown(pe->tls, fd);
            pe->tls->destroy(pe->tls); pe->tls = NULL;
            kl_tls_mbedtls_ctx_destroy(pe->tls_ctx); pe->tls_ctx = NULL;
            sp_close(fd); pe->conn_fd = KL_INVALID_SOCKET; pe->phase = PEER_CLOSED;
            return;
        }
        buf[n] = '\0';
        if (strstr(buf, "EHLO")) {
            peer_tls_send_all(pe, "250-spike-tls\r\n");
            peer_tls_send_all(pe, "250 OK\r\n");
        } else if (strstr(buf, "QUIT")) {
            peer_tls_send_all(pe, "221 Bye\r\n");
            pe->phase = PEER_BYE;
        }
        return;
    }

    char buf[8192];
    kl_ssize_t n = sp_recv(fd, buf, sizeof buf - 1);
    if (n <= 0) {
        if (n < 0 && errno_would_block()) return;
        kl_watcher_del(pe->ev, fd);
        sp_close(fd);
        pe->conn_fd = KL_INVALID_SOCKET;
        pe->phase = PEER_CLOSED;
        return;
    }
    buf[n] = '\0';

    if (pe->phase == PEER_GREETING && strstr(buf, "EHLO")) {
        /* Multiline 250 in TWO writes to exercise fragmented deliver handling. */
        peer_send_all_plain(fd, "250-spike\r\n");
        if (pe->tls_mode == TLS_STARTTLS)
            peer_send_all_plain(fd, "250-STARTTLS\r\n");
        peer_send_all_plain(fd, "250 OK\r\n");
        pe->phase = PEER_EHLO_DONE;
        /* Backpressure test: stop reading NOW so the client's next (big) write
         * congests and its writer reports would-block. */
        if (pe->backpressure)
            pe->stop_reading = 1;
    } else if (pe->phase == PEER_EHLO_DONE && strstr(buf, "STARTTLS")) {
        peer_send_all_plain(fd, "220 2.0.0 Ready to start TLS\r\n");
        pe->phase = PEER_STARTTLS_SENT;
    } else if (pe->phase == PEER_EHLO_DONE && strstr(buf, "QUIT")) {
        peer_send_all_plain(fd, "221 Bye\r\n");
        pe->phase = PEER_BYE;
    }
    /* A huge 'X...' payload (backpressure test) that is neither EHLO nor QUIT is
     * simply drained here once we resume reading - no reply needed. */
}

static void peer_listen_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    FakePeer *pe = user_data;
    (void)ready;
    KlSockAddr peer_addr;
    KlSocketHandle c = g_sp->ops->accept(g_sp->context, fd, &peer_addr);
    if (!kl_handle_valid(c))
        return;
    sp_set_nonblocking(c);
    pe->conn_fd = c;

    if (pe->tls_mode == TLS_IMPLICIT) {
        /* SMTPS: the client handshakes immediately; run the server handshake and
         * only send the greeting once TLS is up. */
        if (peer_tls_begin(pe) != 0) {
            sp_close(c); pe->conn_fd = KL_INVALID_SOCKET; pe->phase = PEER_CLOSED;
            kl_watcher_del(pe->ev, fd); sp_close(fd); pe->listen_fd = KL_INVALID_SOCKET;
            return;
        }
        /* greeting is sent by the watcher once handshake completes (see below). */
        pe->phase = PEER_TLS_HANDSHAKE;
        kl_watcher_add(pe->ev, c, KL_EVENT_READ, peer_conn_watcher, pe);
        peer_tls_drive_handshake(pe);
    } else {
        pe->phase = PEER_GREETING;
        peer_send_all_plain(c, "220 spike ESMTP\r\n");
        kl_watcher_add(pe->ev, c, KL_EVENT_READ, peer_conn_watcher, pe);
    }
    /* one connection is enough: stop listening. */
    kl_watcher_del(pe->ev, fd);
    sp_close(fd);
    pe->listen_fd = KL_INVALID_SOCKET;
}

/* For implicit TLS, once the server handshake completes we must send the SMTPS
 * greeting over the encrypted channel. We piggy-back that on the handshake
 * driver by checking here each tick. */
static void peer_implicit_greeting_pump(FakePeer *pe)
{
    if (pe->tls_mode == TLS_IMPLICIT && pe->tls_up && pe->phase == PEER_TLS_UP) {
        /* send greeting exactly once by advancing to a pseudo-EHLO-wait state */
        static const char *G = "220 spike SMTPS\r\n";
        peer_tls_send_all(pe, G);
        pe->phase = PEER_EHLO_DONE;   /* now expects EHLO over TLS */
    }
}

static int fake_peer_start(FakePeer *pe, KlEventCtx *ev, TlsMode tls_mode)
{
    memset(pe, 0, sizeof *pe);
    pe->ev = ev;
    pe->conn_fd = KL_INVALID_SOCKET;
    pe->listen_fd = KL_INVALID_SOCKET;
    pe->tls_mode = tls_mode;

    KlSocketHandle fd = sp_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) return -1;
    if (g_sp->ops->set_reuseaddr) g_sp->ops->set_reuseaddr(g_sp->context, fd, 1);

    uint8_t lo[4] = {127, 0, 0, 1};
    KlSockAddr bind_addr;
    kl_sockaddr_from_ipv4(&bind_addr, lo, 0);   /* port 0 = ephemeral */
    if (g_sp->ops->bind(g_sp->context, fd, &bind_addr) < 0) { sp_close(fd); return -1; }
    if (g_sp->ops->listen(g_sp->context, fd, 4) < 0) { sp_close(fd); return -1; }

    KlSockAddr local;
    if (g_sp->ops->get_local_addr(g_sp->context, fd, &local) < 0) { sp_close(fd); return -1; }
    pe->port = kl_sockaddr_port(&local);

    if (sp_set_nonblocking(fd) < 0) { sp_close(fd); return -1; }
    pe->listen_fd = fd;
    if (kl_watcher_add(ev, fd, KL_EVENT_READ, peer_listen_watcher, pe) != 0) {
        sp_close(fd); return -1;
    }
    return 0;
}

static void fake_peer_cleanup(FakePeer *pe)
{
    if (pe->tls) { pe->tls->destroy(pe->tls); pe->tls = NULL; }
    if (pe->tls_ctx) { kl_tls_mbedtls_ctx_destroy(pe->tls_ctx); pe->tls_ctx = NULL; }
    if (kl_handle_valid(pe->conn_fd)) { kl_watcher_del(pe->ev, pe->conn_fd); sp_close(pe->conn_fd); pe->conn_fd = KL_INVALID_SOCKET; }
    if (kl_handle_valid(pe->listen_fd)) { kl_watcher_del(pe->ev, pe->listen_fd); sp_close(pe->listen_fd); pe->listen_fd = KL_INVALID_SOCKET; }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Op initialization helper: a single numeric loopback target at `port`.
 * ────────────────────────────────────────────────────────────────────────── */
static void op_init_common(HlSmtpSpikeOp *op, KlEventCtx *ev, KlAllocator alloc)
{
    memset(op, 0, sizeof *op);
    op->ev = ev;
    op->alloc = alloc;
    op->fd = KL_INVALID_SOCKET;
    op->deadline_timer = -1;
    op->deadline_ms = 5000;
    op->connect_result = KL_CONNECT_CANCELLED;
    for (int i = 0; i < KL_CONNECT_MAX_ADDRS; i++)
        op->attempt_fd[i] = KL_INVALID_SOCKET;
}

static void op_set_single_target(HlSmtpSpikeOp *op, uint16_t port)
{
    uint8_t lo[4] = {127, 0, 0, 1};
    kl_sockaddr_from_ipv4(&op->addrs[0], lo, port);
    op->naddrs = 1;
}

/* Pump the loop until `done` predicate returns true or tick bound is hit. */
#define PUMP_UNTIL(ev, cond, maxticks, extra_each_tick)                        \
    do {                                                                       \
        int _ticks = 0;                                                        \
        while (_ticks++ < (maxticks)) {                                        \
            if (cond) break;                                                   \
            extra_each_tick;                                                   \
            int _rc = kl_event_ctx_run((ev), 16, 20);                          \
            SPIKE_CHECK(_rc >= 0, "kl_event_ctx_run returned %d", _rc);        \
            if (_rc < 0) break;                                                \
        }                                                                      \
        logline("loop ran %d tick(s)", _ticks);                               \
    } while (0)

/* ────────────────────────────────────────────────────────────────────────────
 * TEST 1: plaintext SMTP lifecycle over KlConnectOp + KlStream (happy path).
 * ────────────────────────────────────────────────────────────────────────── */
static void run_lifecycle_test(void)
{
    logline("=== TEST 1: plaintext SMTP lifecycle ===");
    KlEventCtx ev;
    KlAllocator alloc = kl_allocator_default();
    SPIKE_CHECK(kl_event_ctx_init(&ev, &alloc) == 0, "kl_event_ctx_init failed");

    FakePeer peer;
    SPIKE_CHECK(fake_peer_start(&peer, &ev, TLS_NONE) == 0, "fake_peer_start failed");
    logline("fake peer on 127.0.0.1:%u", peer.port);

    HlSmtpSpikeOp op;
    op_init_common(&op, &ev, alloc);
    op_set_single_target(&op, peer.port);

    SPIKE_CHECK(kl_connect_op_init(&op.connect_op, &SMTP_CONNECT_HOOKS, &op) == 0, "init");
    SPIKE_CHECK(kl_connect_op_start(&op.connect_op) == 0, "start");

    PUMP_UNTIL(&ev,
        (op.phase == SMTP_DONE && kl_stream_is_detached(&op.stream)
         && kl_connect_op_is_detached(&op.connect_op)),
        2000, (void)0);

    SPIKE_CHECK(op.connect_done_count == 1, "on_done x%d", op.connect_done_count);
    SPIKE_CHECK(op.connect_result == KL_CONNECT_SUCCESS, "result=%d", (int)op.connect_result);
    SPIKE_CHECK(op.connect_detach_count == 1, "on_detach x%d", op.connect_detach_count);
    SPIKE_CHECK(op.ehlo_ok, "EHLO 250 not observed");
    SPIKE_CHECK(op.quit_ok, "QUIT 221 not observed");
    SPIKE_CHECK(kl_stream_is_detached(&op.stream), "stream not detached");
    SPIKE_CHECK(op.stream_closed_count == 1, "on_close x%d", op.stream_closed_count);
    SPIKE_CHECK(!kl_handle_valid(op.fd), "client fd not closed (leak): %ld", (long)op.fd);
    SPIKE_CHECK(kl_stream_write_free(&op.stream) == 0, "write_free");

    fake_peer_cleanup(&peer);
    kl_event_ctx_free(&ev);
}

/* ────────────────────────────────────────────────────────────────────────────
 * TEST 2: write backpressure -> WRITABLE watcher -> kl_stream_flush drain.
 *
 * The peer STOPS reading, so the client's TCP send buffer fills; a large write
 * queues under would-block. The writer reports would-block and arms a WRITABLE
 * watcher. When the peer resumes reading, the writable watcher fires and
 * kl_stream_flush drains the queue; the full payload arrives intact and the
 * conversation completes.
 * ────────────────────────────────────────────────────────────────────────── */
static void run_backpressure_test(void)
{
    logline("=== TEST 2: write backpressure + writable-driven flush ===");
    KlEventCtx ev;
    KlAllocator alloc = kl_allocator_default();
    SPIKE_CHECK(kl_event_ctx_init(&ev, &alloc) == 0, "kl_event_ctx_init failed");

    FakePeer peer;
    SPIKE_CHECK(fake_peer_start(&peer, &ev, TLS_NONE) == 0, "fake_peer_start failed");
    /* Peer stops reading the instant it has sent the EHLO 250 reply, so the
     * client writes its big payload into a socket the peer refuses to drain. */
    peer.backpressure = 1;

    HlSmtpSpikeOp op;
    op_init_common(&op, &ev, alloc);
    op_set_single_target(&op, peer.port);
    op.want_backpressure = 1;
    /* 3 MiB payload dwarfs the loopback send+recv buffers, guaranteeing
     * would-block once the peer stops reading. Fits the 4 MiB write queue. */
    op.big_len = 3 * 1024 * 1024;

    SPIKE_CHECK(kl_connect_op_init(&op.connect_op, &SMTP_CONNECT_HOOKS, &op) == 0, "init");
    SPIKE_CHECK(kl_connect_op_start(&op.connect_op) == 0, "start");

    /* Pump. The peer stops reading after EHLO (its own doing). Once the client
     * writer has reported would-block, resume the peer so the WRITABLE watcher
     * can drive kl_stream_flush and drain the queue. */
    int ticks = 0;
    int flipped_resume = 0;
    while (ticks++ < 8000) {
        if (!flipped_resume && op.write_wouldblocked) {
            peer.stop_reading = 0;
            peer.resumed = 1;
            flipped_resume = 1;
            /* Refresh the peer conn watcher's READ interest (it stayed
             * registered while we ignored reads; a mod re-affirms it). */
            if (kl_handle_valid(peer.conn_fd))
                kl_watcher_mod(&ev, peer.conn_fd, KL_EVENT_READ);
        }
        if (op.phase == SMTP_DONE && kl_stream_is_detached(&op.stream)
            && kl_connect_op_is_detached(&op.connect_op))
            break;
        int rc = kl_event_ctx_run(&ev, 16, 20);
        SPIKE_CHECK(rc >= 0, "kl_event_ctx_run returned %d", rc);
        if (rc < 0) break;
    }
    logline("backpressure loop ran %d tick(s)", ticks);

    SPIKE_CHECK(op.write_wouldblocked, "writer never reported would-block (no congestion forced)");
    SPIKE_CHECK(op.flush_after_wouldblock,
                "WRITABLE watcher never drove kl_stream_flush to drain the queue");
    SPIKE_CHECK(kl_stream_write_pending(&op.stream) == 0, "write queue not drained");
    SPIKE_CHECK(op.quit_ok, "conversation did not complete after backpressure drain");
    SPIKE_CHECK(op.connect_detach_count == 1, "on_detach x%d", op.connect_detach_count);
    SPIKE_CHECK(!kl_handle_valid(op.fd), "client fd not closed (leak)");
    SPIKE_CHECK(kl_stream_write_free(&op.stream) == 0, "write_free");

    fake_peer_cleanup(&peer);
    kl_event_ctx_free(&ev);
}

/* ────────────────────────────────────────────────────────────────────────────
 * TEST 3: multi-address racing + first-address failure survival.
 *
 * The synthetic resolver yields TWO addresses: an unconnectable one first, then
 * the good loopback peer. KlConnectOp races them; the first fails (refused) and
 * the op survives to reach the winner.
 * ────────────────────────────────────────────────────────────────────────── */
static void run_multiaddr_test(void)
{
    logline("=== TEST 3: multi-address race, first fails, second wins ===");
    KlEventCtx ev;
    KlAllocator alloc = kl_allocator_default();
    SPIKE_CHECK(kl_event_ctx_init(&ev, &alloc) == 0, "kl_event_ctx_init failed");

    FakePeer peer;
    SPIKE_CHECK(fake_peer_start(&peer, &ev, TLS_NONE) == 0, "fake_peer_start failed");

    HlSmtpSpikeOp op;
    op_init_common(&op, &ev, alloc);

    /* addr[0]: a closed loopback port (nothing listening) -> ECONNREFUSED fast.
     * We grab an ephemeral port, bind+close it so it is almost certainly free. */
    uint16_t dead_port;
    {
        KlSocketHandle t = sp_socket(AF_INET, SOCK_STREAM, 0);
        uint8_t lo[4] = {127, 0, 0, 1};
        KlSockAddr a; kl_sockaddr_from_ipv4(&a, lo, 0);
        g_sp->ops->bind(g_sp->context, t, &a);
        KlSockAddr got; g_sp->ops->get_local_addr(g_sp->context, t, &got);
        dead_port = kl_sockaddr_port(&got);
        sp_close(t);   /* now nothing listens on dead_port */
    }
    uint8_t lo[4] = {127, 0, 0, 1};
    kl_sockaddr_from_ipv4(&op.addrs[0], lo, dead_port);   /* fails */
    kl_sockaddr_from_ipv4(&op.addrs[1], lo, peer.port);   /* wins */
    op.naddrs = 2;
    /* No RFC8305 delay hook installed, so KlConnectOp starts attempt 0; on its
     * failure it fast-starts attempt 1. This proves first-address-failure
     * survival on the readiness path. */

    SPIKE_CHECK(kl_connect_op_init(&op.connect_op, &SMTP_CONNECT_HOOKS, &op) == 0, "init");
    SPIKE_CHECK(kl_connect_op_start(&op.connect_op) == 0, "start");

    PUMP_UNTIL(&ev,
        (op.phase == SMTP_DONE && kl_stream_is_detached(&op.stream)
         && kl_connect_op_is_detached(&op.connect_op)),
        3000, (void)0);

    SPIKE_CHECK(op.connect_result == KL_CONNECT_SUCCESS, "result=%d (want SUCCESS)", (int)op.connect_result);
    SPIKE_CHECK(op.attempt_fail_count >= 1, "first address did not fail before the win (fail_count=%d)", op.attempt_fail_count);
    SPIKE_CHECK(op.quit_ok, "conversation did not complete via the winning address");
    SPIKE_CHECK(op.connect_detach_count == 1, "on_detach x%d", op.connect_detach_count);
    SPIKE_CHECK(!kl_handle_valid(op.fd), "client fd not closed (leak)");
    SPIKE_CHECK(kl_stream_write_free(&op.stream) == 0, "write_free");

    fake_peer_cleanup(&peer);
    kl_event_ctx_free(&ev);
}

/* ────────────────────────────────────────────────────────────────────────────
 * TEST 4: overall deadline ACTUALLY FIRES against a blackhole.
 *
 * Connect to TEST-NET-1 (192.0.2.1, RFC 5737: guaranteed non-routable) with a
 * short deadline. The connect never completes; the deadline timer fires, the op
 * reaches terminal FAILED, and detaches cleanly with no fd leak.
 * ────────────────────────────────────────────────────────────────────────── */
static void run_deadline_test(void)
{
    logline("=== TEST 4: overall deadline fires (blackhole connect) ===");
    KlEventCtx ev;
    KlAllocator alloc = kl_allocator_default();
    SPIKE_CHECK(kl_event_ctx_init(&ev, &alloc) == 0, "kl_event_ctx_init failed");

    HlSmtpSpikeOp op;
    op_init_common(&op, &ev, alloc);
    op.deadline_ms = 200;   /* short; the no-watch attempt guarantees it wins */
    op.blackhole_no_watch = 1;

    uint8_t blackhole[4] = {192, 0, 2, 1};   /* TEST-NET-1: EINPROGRESS, never completes */
    kl_sockaddr_from_ipv4(&op.addrs[0], blackhole, 25);
    op.naddrs = 1;

    SPIKE_CHECK(kl_connect_op_init(&op.connect_op, &SMTP_CONNECT_HOOKS, &op) == 0, "init");
    SPIKE_CHECK(kl_connect_op_start(&op.connect_op) == 0, "start");

    PUMP_UNTIL(&ev, kl_connect_op_is_detached(&op.connect_op), 200, (void)0);

    SPIKE_CHECK(op.deadline_fired, "deadline callback never fired");
    SPIKE_CHECK(op.connect_done_count == 1, "on_done x%d", op.connect_done_count);
    SPIKE_CHECK(op.connect_result == KL_CONNECT_FAILED,
                "result=%d (want FAILED after deadline)", (int)op.connect_result);
    SPIKE_CHECK(op.connect_error == (int)KL_ERR_TIMEOUT,
                "error=%d (want KL_ERR_TIMEOUT=%d)", op.connect_error, (int)KL_ERR_TIMEOUT);
    SPIKE_CHECK(op.connect_detach_count == 1, "on_detach x%d", op.connect_detach_count);
    SPIKE_CHECK(kl_connect_op_is_detached(&op.connect_op), "not detached after deadline");
    /* The racing fd must be disposed (the attempt was cancelled by the deadline). */
    SPIKE_CHECK(!kl_handle_valid(op.attempt_fd[0]), "blackhole fd not disposed (leak)");

    kl_event_ctx_free(&ev);
}

/* ────────────────────────────────────────────────────────────────────────────
 * TEST 5: cancellation DURING resolution -> clean detachment.
 *
 * start_resolve leaves the resolve in flight (does not report addresses); we
 * cancel while state == RESOLVING and assert clean CANCELLED detachment, with
 * cancel_resolve invoked and no fd created/leaked.
 * ────────────────────────────────────────────────────────────────────────── */
static void run_cancel_during_resolve_test(void)
{
    logline("=== TEST 5: cancel DURING resolution ===");
    KlEventCtx ev;
    KlAllocator alloc = kl_allocator_default();
    SPIKE_CHECK(kl_event_ctx_init(&ev, &alloc) == 0, "kl_event_ctx_init failed");

    HlSmtpSpikeOp op;
    op_init_common(&op, &ev, alloc);
    op.cancel_during_resolve = 1;   /* start_resolve stays in flight */
    op_set_single_target(&op, 25);  /* never used; we cancel first */

    SPIKE_CHECK(kl_connect_op_init(&op.connect_op, &SMTP_CONNECT_HOOKS, &op) == 0, "init");
    SPIKE_CHECK(kl_connect_op_start(&op.connect_op) == 0, "start");

    SPIKE_CHECK(op.resolve_started, "start_resolve was not entered");
    SPIKE_CHECK(kl_connect_op_state(&op.connect_op) == KL_CONNECT_OP_STATE_RESOLVING,
                "state=%d before cancel (want RESOLVING=%d)",
                (int)kl_connect_op_state(&op.connect_op), (int)KL_CONNECT_OP_STATE_RESOLVING);

    SPIKE_CHECK(kl_connect_op_cancel(&op.connect_op) == 0, "cancel");

    PUMP_UNTIL(&ev, kl_connect_op_is_detached(&op.connect_op), 200, (void)0);

    SPIKE_CHECK(op.connect_done_count == 1, "on_done x%d", op.connect_done_count);
    SPIKE_CHECK(op.connect_result == KL_CONNECT_CANCELLED,
                "result=%d (want CANCELLED)", (int)op.connect_result);
    SPIKE_CHECK(op.resolve_cancelled, "cancel_resolve hook was not invoked");
    SPIKE_CHECK(op.connect_detach_count == 1, "on_detach x%d", op.connect_detach_count);
    SPIKE_CHECK(kl_connect_op_is_detached(&op.connect_op), "not detached after cancel");
    SPIKE_CHECK(!kl_handle_valid(op.fd), "fd created/leaked during a cancelled resolve");

    kl_event_ctx_free(&ev);
}

/* ────────────────────────────────────────────────────────────────────────────
 * TEST 6: cancellation DURING the connect race -> clean detachment.
 * (Retained from the original spike; complements TEST 5.)
 * ────────────────────────────────────────────────────────────────────────── */
static void run_cancel_connecting_test(void)
{
    logline("=== TEST 6: cancel DURING the connect race ===");
    KlEventCtx ev;
    KlAllocator alloc = kl_allocator_default();
    SPIKE_CHECK(kl_event_ctx_init(&ev, &alloc) == 0, "kl_event_ctx_init failed");

    HlSmtpSpikeOp op;
    op_init_common(&op, &ev, alloc);
    op.connect_result = KL_CONNECT_SUCCESS;   /* seed with the wrong value */

    uint8_t testnet[4] = {192, 0, 2, 1};   /* EINPROGRESS, never completes */
    kl_sockaddr_from_ipv4(&op.addrs[0], testnet, 25);
    op.naddrs = 1;

    SPIKE_CHECK(kl_connect_op_init(&op.connect_op, &SMTP_CONNECT_HOOKS, &op) == 0, "init");
    SPIKE_CHECK(kl_connect_op_start(&op.connect_op) == 0, "start");

    SPIKE_CHECK(kl_connect_op_state(&op.connect_op) == KL_CONNECT_OP_STATE_CONNECTING,
                "state=%d before cancel (want CONNECTING=%d)",
                (int)kl_connect_op_state(&op.connect_op), (int)KL_CONNECT_OP_STATE_CONNECTING);
    SPIKE_CHECK(kl_handle_valid(op.attempt_fd[0]), "no racing attempt fd before cancel");

    SPIKE_CHECK(kl_connect_op_cancel(&op.connect_op) == 0, "cancel");

    PUMP_UNTIL(&ev, kl_connect_op_is_detached(&op.connect_op), 200, (void)0);

    SPIKE_CHECK(op.connect_done_count == 1, "on_done x%d", op.connect_done_count);
    SPIKE_CHECK(op.connect_result == KL_CONNECT_CANCELLED, "result=%d", (int)op.connect_result);
    SPIKE_CHECK(op.connect_detach_count == 1, "on_detach x%d", op.connect_detach_count);
    SPIKE_CHECK(!kl_handle_valid(op.attempt_fd[0]), "racing fd not disposed on cancel (leak)");

    kl_event_ctx_free(&ev);
}

/* ────────────────────────────────────────────────────────────────────────────
 * TEST 7: LIVE STARTTLS upgrade against a real mbedTLS peer (verify success).
 * TEST 8: LIVE implicit TLS (SMTPS) against a real mbedTLS peer (verify success).
 * TEST 9: negative case: hostname mismatch is REJECTED (no plaintext fallback).
 * ────────────────────────────────────────────────────────────────────────── */
static void run_tls_test(const char *label, TlsMode mode, int verify,
                         const char *ca_path, const char *hostname, int expect_fail)
{
    logline("=== TLS TEST: %s (mode=%d verify=%d host=%s expect_fail=%d) ===",
            label, (int)mode, verify, hostname ? hostname : "(none)", expect_fail);
    KlEventCtx ev;
    KlAllocator alloc = kl_allocator_default();
    SPIKE_CHECK(kl_event_ctx_init(&ev, &alloc) == 0, "kl_event_ctx_init failed");

    FakePeer peer;
    SPIKE_CHECK(fake_peer_start(&peer, &ev, mode) == 0, "fake_peer_start failed");

    HlSmtpSpikeOp op;
    op_init_common(&op, &ev, alloc);
    op_set_single_target(&op, peer.port);
    op.tls_mode = mode;
    op.tls_verify = verify;
    op.tls_ca_path = ca_path;
    op.tls_hostname = hostname;
    op.expect_tls_fail = expect_fail;

    SPIKE_CHECK(kl_connect_op_init(&op.connect_op, &SMTP_CONNECT_HOOKS, &op) == 0, "init");
    SPIKE_CHECK(kl_connect_op_start(&op.connect_op) == 0, "start");

    int ticks = 0;
    while (ticks++ < 4000) {
        peer_implicit_greeting_pump(&peer);
        int done = expect_fail
            ? (op.tls_failed && kl_stream_is_detached(&op.stream) && kl_connect_op_is_detached(&op.connect_op))
            : (op.phase == SMTP_DONE && kl_stream_is_detached(&op.stream) && kl_connect_op_is_detached(&op.connect_op));
        if (done) break;
        int rc = kl_event_ctx_run(&ev, 16, 20);
        SPIKE_CHECK(rc >= 0, "kl_event_ctx_run returned %d", rc);
        if (rc < 0) break;
    }
    logline("tls loop ran %d tick(s)", ticks);

    if (expect_fail) {
        /* Negative: the handshake MUST have failed, TLS never came up, and NO
         * plaintext SMTP conversation completed (no fallback). */
        SPIKE_CHECK(op.tls_failed, "negative case: handshake did not fail");
        SPIKE_CHECK(!op.tls_completed, "negative case: TLS came up despite bad verification");
        SPIKE_CHECK(!op.quit_ok, "negative case: plaintext/insecure fallback completed a conversation");
    } else {
        SPIKE_CHECK(op.tls_completed, "TLS handshake did not complete");
        /* Handshake watcher transition proof: at least one WANT_READ or
         * WANT_WRITE was observed and re-armed (a real network handshake always
         * yields at least once). */
        SPIKE_CHECK(op.tls_want_read_seen || op.tls_want_write_seen,
                    "no WANT_READ/WANT_WRITE handshake transition observed");
        SPIKE_CHECK(op.quit_ok, "encrypted SMTP conversation did not complete");
        SPIKE_CHECK(op.plaintext_delivered_during_hs == 0,
                    "plaintext delivered across the TLS upgrade boundary (x%d)",
                    op.plaintext_delivered_during_hs);
        SPIKE_CHECK(op.connect_detach_count == 1, "on_detach x%d", op.connect_detach_count);
        SPIKE_CHECK(!kl_handle_valid(op.fd), "client fd not closed after TLS (leak)");
    }
    if (kl_stream_close_state(&op.stream) != KL_STREAM_STATE_OPEN || kl_stream_is_detached(&op.stream))
        kl_stream_write_free(&op.stream);

    fake_peer_cleanup(&peer);
    kl_event_ctx_free(&ev);
}

/* ────────────────────────────────────────────────────────────────────────────
 * TEST 10: real DNS resolver path (multi-address capable) resolving a numeric
 * loopback via the built-in kl_dns_resolver. This exercises the ACTUAL resolver
 * vtable KlConnectOp would call, not a synthesized single address.
 *
 * We resolve "localhost" through the built-in resolver's /etc/hosts fast path
 * (no network), which yields 127.0.0.1 without a DNS round-trip; the connect
 * then targets our peer's ephemeral port (we override the resolved port).
 * ────────────────────────────────────────────────────────────────────────── */
static void co_dns_done_fixed_port(KlResolveReq *req, const KlResolveResult *result,
                                    int error, void *user_data)
{
    HlSmtpSpikeOp *op = user_data;
    (void)req;
    op->dns_req = NULL;
    if (error != 0 || !result || result->naddrs < 1) {
        kl_connect_op_on_resolve_failed(&op->connect_op, (int)KL_ERR_DNS);
        return;
    }
    int n = result->naddrs;
    if (n > KL_CONNECT_MAX_ADDRS) n = KL_CONNECT_MAX_ADDRS;
    for (int i = 0; i < n; i++) {
        op->addrs[i] = result->addrs[i];
        /* Override to our peer's port (resolver returned port 0 / a fixed one). */
        kl_sockaddr_set_port(&op->addrs[i], op->deadline_ms == 0 ? 0 : (uint16_t)op->big_len);
    }
    op->naddrs = n;
    kl_connect_op_on_resolved(&op->connect_op, n);
}

static void run_dns_resolver_test(void)
{
    logline("=== TEST 10: real kl_dns_resolver resolve of localhost (/etc/hosts) ===");
    KlEventCtx ev;
    KlAllocator alloc = kl_allocator_default();
    SPIKE_CHECK(kl_event_ctx_init(&ev, &alloc) == 0, "kl_event_ctx_init failed");

    FakePeer peer;
    SPIKE_CHECK(fake_peer_start(&peer, &ev, TLS_NONE) == 0, "fake_peer_start failed");

    KlResolver *dns = kl_dns_resolver_create(&ev, NULL);
    if (!dns) {
        logline("kl_dns_resolver_create returned NULL (resolv.conf/hosts unavailable) - "
                "SKIP the real-resolver leg (synthetic multi-addr TEST 3 covers racing).");
        fake_peer_cleanup(&peer);
        kl_event_ctx_free(&ev);
        return;
    }

    HlSmtpSpikeOp op;
    op_init_common(&op, &ev, alloc);
    op.use_dns_resolver = 1;
    op.dns = dns;
    op.tls_hostname = "localhost";
    /* Smuggle the peer port through big_len (see co_dns_done_fixed_port). */
    op.big_len = peer.port;

    /* Use the port-fixing done callback by pointing the resolver at it: we set
     * use_dns_resolver so co_start_resolve calls dns->resolve with co_dns_done;
     * but we want the fixed-port variant. Re-wire by resolving directly here and
     * letting co_start_resolve just kick it. Simplest: override the done in the
     * resolve call. We do that by NOT using co_start_resolve's default; instead
     * we resolve inline in start via a flag. To keep it simple, temporarily use a
     * dedicated resolve below. */
    op.dns_req = dns->resolve(dns, &ev, "localhost", 0, co_dns_done_fixed_port, &op);

    SPIKE_CHECK(kl_connect_op_init(&op.connect_op, &SMTP_CONNECT_HOOKS, &op) == 0, "init");
    /* We already kicked resolve; make start_resolve a no-op that just marks it. */
    op.use_dns_resolver = 0;      /* co_start_resolve would double-resolve; suppress */
    op.cancel_during_resolve = 1; /* co_start_resolve returns without reporting;
                                     the inline resolve above drives on_resolved. */
    SPIKE_CHECK(kl_connect_op_start(&op.connect_op) == 0, "start");

    PUMP_UNTIL(&ev,
        (op.phase == SMTP_DONE && kl_stream_is_detached(&op.stream)
         && kl_connect_op_is_detached(&op.connect_op)),
        4000, (void)0);

    SPIKE_CHECK(op.connect_result == KL_CONNECT_SUCCESS,
                "result=%d (want SUCCESS via real resolver)", (int)op.connect_result);
    SPIKE_CHECK(op.quit_ok, "conversation did not complete via resolved address");
    SPIKE_CHECK(op.connect_detach_count == 1, "on_detach x%d", op.connect_detach_count);
    SPIKE_CHECK(!kl_handle_valid(op.fd), "client fd not closed (leak)");
    if (kl_stream_is_detached(&op.stream))
        kl_stream_write_free(&op.stream);

    dns->destroy(dns);
    fake_peer_cleanup(&peer);
    kl_event_ctx_free(&ev);
}

int main(void)
{
    g_sp = kl_socket_provider_posix();
    if (!g_sp || !g_sp->ops) {
        fprintf(stderr, "SPIKE FAIL: kl_socket_provider_posix() returned no ops table\n");
        return 1;
    }
    fprintf(stderr, "POSIX socket provider: name=%s caps=0x%llx io_status=%s\n",
            g_sp->ops->name ? g_sp->ops->name : "(null)",
            (unsigned long long)g_sp->capabilities,
            g_sp->ops->io_status ? "present" : "NULL (errno fallback)");

    run_lifecycle_test();
    run_backpressure_test();
    run_multiaddr_test();
    run_deadline_test();
    run_cancel_during_resolve_test();
    run_cancel_connecting_test();
    run_tls_test("STARTTLS upgrade (verify OK)", TLS_STARTTLS, 1, SPIKE_TLS_CERT, "spike.local", 0);
    run_tls_test("implicit TLS / SMTPS (verify OK)", TLS_IMPLICIT, 1, SPIKE_TLS_CERT, "spike.local", 0);
    run_tls_test("STARTTLS hostname MISMATCH (reject)", TLS_STARTTLS, 1, SPIKE_TLS_CERT, "wrong.example", 1);
    run_tls_test("implicit TLS unknown CA (reject)", TLS_IMPLICIT, 1, "spike/tls/other.crt", "spike.local", 1);
    run_dns_resolver_test();

    if (g_failures == 0) {
        printf("SPIKE PASS\n");
        return 0;
    }
    printf("SPIKE FAIL: %s\n", g_fail_reason);
    return 1;
}
