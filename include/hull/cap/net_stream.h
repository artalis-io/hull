/**
 * @file cap/net_stream.h
 * @brief Outbound byte stream: the transport half of hull/net.
 *
 * Authorization lives next door in cap/net.h and runs BEFORE anything here is
 * reachable. This header owns only bytes: connect, read, write, close, cancel,
 * deadline. It knows no protocol.
 *
 * ## Scheduling
 *
 * Event-loop integrated, which is what separates this from Hull's two existing
 * transports. HlDbTransport pumps a private loop during connect then blocks on
 * a worker; HlSmtpTransport pumps its own loop for a whole conversation on a
 * pool worker. Both pin a thread for the life of the operation, which is fine
 * for a request-scoped send and wrong for a long-lived connection: the default
 * pool is 4 workers, so a handful of idle SSH sessions would exhaust it.
 *
 * So this schedules through HlAsyncBackend (watchers, timers, op suspend), the
 * seam that works for a server app and an `app.main` CLI app alike. Connect
 * borrows the backend's KlEventCtx for KlConnectOp, which brings resolve,
 * IPv4/IPv6 racing, per-attempt delay and a connect deadline that this file
 * does not have to reinvent. See docs/net_module_design.md section 6a.
 *
 * ## Ownership, which is the part that bites
 *
 * The stream is a HEAP allocation behind an opaque handle, never embedded in a
 * caller's struct. That is not a style choice. A KlConnectOp that has not
 * confirmed detachment still references its own storage, so that storage must
 * not be freed under it; only a separately-owned heap block can be abandoned
 * whole on the exceptional non-detachment path. Both existing transports reach
 * the same conclusion, for the same reason.
 *
 * Consequences the caller must respect:
 *
 *   - hl_net_stream_free() may DEFER the actual free until the connect op
 *     detaches. It always returns promptly; the handle is dead either way.
 *   - Cancellation does not return ownership until the connect op and every
 *     racing descriptor and timer are detached.
 *   - After close or cancel, every operation fails closed with HL_NET_E_CLOSED.
 *     There is no "half-closed but still readable" state exposed here.
 *
 * ## Read model
 *
 * Reads are PULL (`hl_net_stream_read` asks for up to n bytes) over a PUSH
 * delivery path (the loop hands bytes to a callback whenever they arrive). The
 * gap is bridged by a bounded receive buffer plus the park/resume pattern the
 * multipart body reader already uses: a read that cannot be satisfied parks the
 * caller and the delivery callback resumes it.
 *
 * A read returns what has arrived, never "one protocol record". Framing is the
 * caller's business, which is what lets SSH, and later WebSocket, sit on top
 * without this layer knowing either protocol.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_NET_STREAM_H
#define HULL_CAP_NET_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HlNetStream HlNetStream;

/* Errors. Negative, distinct, and stable: the Lua/JS binding maps these onto
 * the structured codes in docs/ssh_module_design.md section 9, and collapsing
 * any two of them would lose a distinction an application acts on. */
#define HL_NET_OK            0
#define HL_NET_E_DENIED     (-1)   /* capability refused; never reaches here  */
#define HL_NET_E_RESOLVE    (-2)   /* name did not resolve                    */
#define HL_NET_E_CONNECT    (-3)   /* every attempted address failed          */
#define HL_NET_E_TIMEOUT    (-4)   /* deadline expired                        */
#define HL_NET_E_CLOSED     (-5)   /* peer closed, or we did                  */
#define HL_NET_E_CANCELLED  (-6)   /* cancelled by the caller                 */
#define HL_NET_E_IO         (-7)   /* transport error                         */
#define HL_NET_E_NOMEM      (-8)
#define HL_NET_E_INVAL      (-9)   /* bad argument, bounded before use        */

/* Bounds. A hostile or merely slow peer must not be able to grow Hull's
 * memory, so both buffers are fixed at construction and writes are admitted
 * all-or-none against the send capacity rather than queued without limit. */
#define HL_NET_READ_CAP_DEFAULT   (64u * 1024u)
#define HL_NET_WRITE_CAP_DEFAULT  (64u * 1024u)
#define HL_NET_READ_CAP_MAX       (1024u * 1024u)
#define HL_NET_WRITE_CAP_MAX      (1024u * 1024u)

typedef struct HlNetStreamConfig {
    const char *host;          /* borrowed for the duration of the call    */
    int         port;          /* 1..65535, already authorized             */
    int         connect_ms;    /* whole-connect deadline; <=0 = default    */
    size_t      read_cap;      /* 0 = HL_NET_READ_CAP_DEFAULT              */
    size_t      write_cap;     /* 0 = HL_NET_WRITE_CAP_DEFAULT             */
} HlNetStreamConfig;

/**
 * Begin connecting. Returns HL_NET_OK with *out set, or a negative error.
 *
 * Does NOT authorize: the caller must have called hl_cap_net_check_connect
 * first. This split is deliberate (see cap/net.h) so that a denial cannot race
 * a resolution that was never started.
 */
int hl_net_stream_connect(HlNetStream **out, const HlNetStreamConfig *cfg);

/**
 * Read up to `len` bytes. Returns the count (>0), 0 on clean EOF, or a
 * negative error. May park the calling coroutine when nothing is buffered yet.
 */
long hl_net_stream_read(HlNetStream *s, void *buf, size_t len);

/**
 * Write exactly `len` bytes, all-or-none against the send capacity. Returns
 * HL_NET_OK or a negative error. May park while the queue drains; that is
 * backpressure surfacing rather than an error.
 */
int hl_net_stream_write(HlNetStream *s, const void *buf, size_t len);

/** Rearm the operation deadline. <=0 clears it. */
void hl_net_stream_deadline(HlNetStream *s, int ms);

/** Graceful close: drain what is queued, then FIN. Idempotent. */
void hl_net_stream_close(HlNetStream *s);

/** Abortive teardown. Safe mid-connect and mid-handshake. Idempotent. */
void hl_net_stream_cancel(HlNetStream *s);

/**
 * Release the handle. Safe to call exactly once on a live or dead stream; the
 * handle is invalid on return regardless. The underlying allocation may
 * outlive the call if a connect op has not confirmed detachment, which is
 * intentional and is why the stream is never embedded in caller storage.
 */
void hl_net_stream_free(HlNetStream *s);

/** Stable message for an HL_NET_E_* code. Never NULL. */
const char *hl_net_stream_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* HULL_CAP_NET_STREAM_H */
