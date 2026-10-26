/*
 * cap/pg_conn.c: PostgreSQL DSN parsing, connect, and startup handshake
 *
 * A blocking connection with trust / cleartext-password / SCRAM-SHA-256 auth
 * and optional TLS (via KlTls, mirroring cap/smtp.c). The receive path feeds the
 * untrusted pgwire reader; the DSN parser bounds every copy and rejects
 * oversized components rather than truncating.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/pg_conn.h"
#include "hull/cap/pgwire.h"
#include "../utils/base64.h"
#include "../utils/url.h"
/* SCRAM pulls in cap/crypto (mbedTLS). The DSN / rewriter fuzz harnesses,
 * which link this file for its pure functions, define HL_PG_NO_SCRAM to
 * compile crypto-free. */
#ifndef HL_PG_NO_SCRAM
#include "hull/cap/crypto.h"
#endif
/* The byte transport (Keel v3) + the TLS client. The pure-parser fuzzers define
 * HL_PG_NO_TLS to stay free of Keel; the whole connection layer below is wrapped
 * under the same guard, so those harnesses omit it and reach only the pure
 * functions (DSN parse / sslmode parse / SCRAM proof / SQL rewrite). */
#ifndef HL_PG_NO_TLS
#include "hull/cap/db_transport.h"
#include "hull/cap/db_budget.h"     /* hl_db_budget_charge: a tripped run */
#include "hull/cap/db_sql_kw.h"     /* hl_sql_txn_kind_ex: a lone COMMIT */
#include "hull/shared/tls_client.h"
#endif

#include <ctype.h>
#include <errno.h>       /* EINTR in the notify-wait poll loop */
#include <poll.h>        /* poll() in hl_pg_wait_notify */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     /* strcasecmp */
#include <time.h>        /* clock_gettime(CLOCK_MONOTONIC) */
#include <unistd.h>

/* Upper bound on a server-supplied SCRAM iteration count. RFC 5802
 * recommends at least 4096 and Postgres ships exactly that, so this is
 * ~2400x the real-world value while still bounding the work a hostile or
 * MITM'd peer can ask the client to perform. */
#define SCRAM_MAX_ITERS 10000000
/* RFC 7677's floor. A server that asks for fewer is accepted only where a
 * cleartext password would be (verified TLS, or sslmode=disable): over
 * prefer / require a MITM could ask for i=1 and brute-force the password
 * from the ClientProof offline at no cost (audit 29). */
#define SCRAM_MIN_ITERS 4096
/* What one SCRAM PBKDF2 iteration is charged to the run's budget: one 32-byte
 * block, two HMAC-SHA256 compressions of 64 bytes at 1 unit per 8 bytes - the
 * rate the Lua crypto binding charges its own PBKDF2 (audit 17). */
#define HL_PG_SCRAM_UNITS_PER_ITER ((2 * 64) / 8)
/* Stringified for the rejection message, so the text cannot drift
 * from the bound it reports (set_err takes a plain string). */
#define HL_STR2(x) #x
#define HL_STR(x)  HL_STR2(x)


/* Overwrite memory the optimizer cannot elide (secret scrub). */
static void pg_secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static void set_err(char *dst, size_t cap, const char *msg)
{
    if (dst && cap) snprintf(dst, cap, "%s", msg);
}

/* ── DSN parsing ──────────────────────────────────────────────────── */

/* @p p is lowercase; @p s matches it without case. */
static int starts_with_ci(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        unsigned char a = (unsigned char)*s;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (a != (unsigned char)*p) return 0;
    }
    return 1;
}

int hl_pg_dsn_parse(const char *dsn, HlPgDsn *out, char *errbuf, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->port, sizeof out->port, "%s", "5432");
    out->read_timeout_ms = HL_PG_READ_TIMEOUT_DEFAULT_MS;

    if (!dsn) { set_err(errbuf, errlen, "null DSN"); return -1; }

    const char *p = dsn;
    /* The scheme without case: the selector lowercases the one it routes
     * by, so "PostgreSQL://" chose this backend and then failed here. */
    if (starts_with_ci(p, "postgres://"))         p += strlen("postgres://");
    else if (starts_with_ci(p, "postgresql://"))  p += strlen("postgresql://");
    else { set_err(errbuf, errlen, "DSN must start with postgres:// or postgresql://");
           return -1; }

    size_t rest = strlen(p);

    /* Authority runs to the first '/' (path) or '?' (query). */
    size_t auth_len = rest;
    for (size_t i = 0; i < rest; i++) {
        if (p[i] == '/' || p[i] == '?') { auth_len = i; break; }
    }

    /* [userinfo '@'] hostport within the authority. */
    const char *hostport = p;
    size_t hostport_len = auth_len;
    const void *atp = memchr(p, '@', auth_len);
    if (atp) {
        size_t ui_len = (size_t)((const char *)atp - p);
        const void *cp = memchr(p, ':', ui_len);
        if (cp) {
            size_t ulen = (size_t)((const char *)cp - p);
            const char *pw = (const char *)cp + 1;
            size_t plen = ui_len - ulen - 1;
            if (hl_url_decode(p, ulen, out->user, sizeof out->user, 0) < 0 ||
                hl_url_decode(pw, plen, out->password, sizeof out->password, 0) < 0) {
                set_err(errbuf, errlen, "DSN user/password too long or malformed");
                return -1;
            }
        } else if (hl_url_decode(p, ui_len, out->user, sizeof out->user, 0) < 0) {
            set_err(errbuf, errlen, "DSN user too long or malformed");
            return -1;
        }
        hostport = (const char *)atp + 1;
        hostport_len = auth_len - ui_len - 1;
    }

    /* host[:port]. Reject bracketless IPv6 ambiguity for now (needs [..]). */
    const void *hcolon = memchr(hostport, ':', hostport_len);
    size_t host_len = hostport_len;
    if (hcolon) {
        host_len = (size_t)((const char *)hcolon - hostport);
        const char *port = (const char *)hcolon + 1;
        size_t port_len = hostport_len - host_len - 1;
        if (port_len == 0 || port_len >= sizeof out->port) {
            set_err(errbuf, errlen, "DSN port invalid"); return -1;
        }
        for (size_t i = 0; i < port_len; i++) {
            if (port[i] < '0' || port[i] > '9') {
                set_err(errbuf, errlen, "DSN port must be numeric"); return -1;
            }
        }
        memcpy(out->port, port, port_len);
        out->port[port_len] = '\0';
    }
    if (host_len == 0 || hl_url_decode(hostport, host_len,
                                       out->host, sizeof out->host, 0) < 0) {
        set_err(errbuf, errlen, "DSN host missing or too long"); return -1;
    }

    /* Path (dbname) and query string follow the authority. */
    const char *tail = p + auth_len;              /* points at '/' or '?' or '\0' */
    const char *qmark = strchr(tail, '?');
    if (*tail == '/') {
        const char *db = tail + 1;
        size_t db_len = (qmark ? (size_t)(qmark - db) : strlen(db));
        if (hl_url_decode(db, db_len, out->dbname, sizeof out->dbname, 0) < 0) {
            set_err(errbuf, errlen, "DSN database name too long"); return -1;
        }
    }
    if (qmark) {
        /* sslmode and read_timeout are read; other params are ignored. */
        const char *q = qmark + 1;
        while (*q) {
            const char *amp = strchr(q, '&');
            size_t pair_len = amp ? (size_t)(amp - q) : strlen(q);
            const char *eq = memchr(q, '=', pair_len);
            if (eq) {
                size_t klen = (size_t)(eq - q);
                if (klen == strlen("sslmode") &&
                    strncmp(q, "sslmode", klen) == 0) {
                    const char *v = eq + 1;
                    size_t vlen = pair_len - klen - 1;
                    if (hl_url_decode(v, vlen, out->sslmode, sizeof out->sslmode, 0) < 0) {
                        set_err(errbuf, errlen, "DSN sslmode too long"); return -1;
                    }
                } else if (klen == strlen("read_timeout") &&
                           strncmp(q, "read_timeout", klen) == 0) {
                    /* Milliseconds a single read may block; 0 = no limit. */
                    char tb[16];
                    size_t vlen = pair_len - klen - 1;
                    char *end = NULL;
                    long ms = -1;
                    if (vlen > 0 && vlen < sizeof tb) {
                        memcpy(tb, eq + 1, vlen);
                        tb[vlen] = '\0';
                        ms = strtol(tb, &end, 10);
                        if (end == tb || *end != '\0') ms = -1;
                    }
                    if (ms < 0 || ms > 86400000) {
                        set_err(errbuf, errlen, "DSN read_timeout must be 0..86400000 (ms)");
                        return -1;
                    }
                    out->read_timeout_ms = (int)ms;
                }
            }
            if (!amp) break;
            q = amp + 1;
        }
    }

    if (out->user[0] == '\0') {
        set_err(errbuf, errlen, "DSN missing user"); return -1;
    }
    return 0;
}

void hl_pg_dsn_scrub(HlPgDsn *dsn)
{
    if (dsn) pg_secure_zero(dsn->password, sizeof dsn->password);
}

/* ── Connection layer (Keel v3 transport) ─────────────────────────────
 * Everything from here to the end of hl_pg_conn_close (plus the query path and
 * hl_pg_wait_notify further down) rides the HlDbTransport byte transport, which
 * pulls in Keel + tls_client + mbedTLS. The pure-parser fuzzers set HL_PG_NO_TLS
 * to omit this whole layer and keep only the DSN parser, sslmode parser,
 * SCRAM proof, and the SQL rewriter (all outside this guard). */
#ifndef HL_PG_NO_TLS

/* Transport helpers. All bytes go through the owned HlDbTransport, which routes
 * through an attached TLS session when one has been handed to it (see
 * hl_pg_conn_open) or plain provider send/recv otherwise. */
static void pg_abandon(HlPgConn *conn);
static void conn_teardown(HlPgConn *conn);
static int pg_wait_stop(void *ud);
static int notify_is_for(const HlPgFrame *f, const char *channel, char *got,
                         size_t gotsz);

/* Send a whole message. The write is sliced like the reads (audit 23): a
 * blocking send waited for the server to take the bytes under SO_SNDTIMEO
 * alone - for ever with ?read_timeout=0 - so a server that stopped reading a
 * large conn.exec held the event loop with no budget or run deadline asked.
 * A trip closes the connection, as a trip mid-reply does; a timeout or a
 * failed write leaves it broken (part of the message may have gone). */
static int conn_send(HlPgConn *conn, const uint8_t *buf, size_t len)
{
    /* While finishing (no budget asked) the write is held to what is left of
     * the finishing bound too, not ?read_timeout= alone (audit 27). */
    uint64_t started = 0;
    int timeout = hl_pg_conn_io_left(conn, &started);
    if (timeout < 0) { pg_abandon(conn); return -1; }
    int rc = hl_db_transport_send_sliced(conn->transport, buf, len, timeout,
                                         conn->finishing ? NULL : pg_wait_stop,
                                         NULL);
    if (rc == 0) return 0;
    if (rc == -2) {
        conn->broken = 1;      /* first: the teardown sends no close_notify */
        conn_teardown(conn);
        set_err(conn->errmsg, sizeof conn->errmsg,
                "interrupted (instruction limit exceeded); the connection was "
                "closed with the request part-sent");
        return -1;
    }
    conn->broken = 1;
    set_err(conn->errmsg, sizeof conn->errmsg,
            "socket write failed or timed out (see ?read_timeout=)");
    return -1;
}

/* A NotificationResponse a query or a script read: hand it to the backend's
 * listen table (conn->on_notify), as wait_notify does for another channel's.
 * Dropped, the next wait on that channel slept to its timeout (audit 20). */
static void pg_note_notify(HlPgConn *conn, const HlPgFrame *f)
{
    char got[64];
    if (!conn->on_notify) return;
    (void)notify_is_for(f, NULL, got, sizeof got);
    if (got[0]) conn->on_notify(conn->on_notify_ud, got);
}

/* Asked between the slices of a wait for the server (audit 18): a 0-unit
 * charge, which trips once the run's budget is spent or its wall-clock
 * deadline has passed. */
static int pg_wait_stop(void *ud)
{
    (void)ud;
    return hl_db_budget_charge(0) != 0;
}

/* Bound on the whole read that finishes a tripped run's COMMIT reply: the
 * DSN's ?read_timeout=, but never more than this (and this when it is 0). */
#define HL_PG_FINISH_MAX_MS 2000

/* What is left of the finishing bound, -1 once it has passed (never 0). */
static int pg_finish_left(HlPgConn *conn)
{
    int bound = HL_PG_FINISH_MAX_MS;
    if (conn->read_timeout_ms > 0 && conn->read_timeout_ms < bound)
        bound = conn->read_timeout_ms;
    return hl_db_transport_time_left(&conn->finish_started, bound);
}

int hl_pg_conn_io_left(HlPgConn *conn, uint64_t *started)
{
    int left = hl_db_transport_time_left(started, conn->read_timeout_ms);
    if (left < 0 || !conn->finishing)
        return left;
    int fleft = pg_finish_left(conn);
    if (fleft < 0)
        return -1;
    return (left == 0 || fleft < left) ? fleft : left;
}

/* The read buffer starts at HL_PG_RBUF_INITIAL and doubles up to
 * HL_PG_MAX_MSG + 8192 for one large frame; it used to keep that size until
 * the connection closed, so every pooled connection that once read a big row
 * held ~1 GiB for good. Once the large frame is consumed and what is left
 * fits the initial size, it shrinks back (audit 29). */
#define HL_PG_RBUF_INITIAL 8192
#define HL_PG_RBUF_SHRINK  ((size_t)1 << 20)

/* Drop the bytes already consumed, and give a grown buffer back. */
static void pg_rbuf_compact(HlPgConn *conn)
{
    if (conn->consumed > 0) {
        memmove(conn->rbuf, conn->rbuf + conn->consumed,
                conn->rlen - conn->consumed);
        conn->rlen -= conn->consumed;
        conn->consumed = 0;
    }
    if (conn->rcap > HL_PG_RBUF_SHRINK && conn->rlen <= HL_PG_RBUF_INITIAL) {
        uint8_t *nb = realloc(conn->rbuf, HL_PG_RBUF_INITIAL);
        if (nb) {
            conn->rbuf = nb;
            conn->rcap = HL_PG_RBUF_INITIAL;
        }
    }
}

/* The run asked to stop while a reply was awaited. After a lone COMMIT was
 * sent the server commits regardless, so the reply is read on in the bounded
 * finishing mode (0) instead of dropped, which kept tx_status 'T' and
 * reported the commit lost (audit 29); otherwise the connection is
 * abandoned (-1). */
static int pg_stop_or_finish(HlPgConn *conn)
{
    if (conn->commit_sent && !conn->finishing) {
        conn->finishing = 1;
        conn->finish_started = 0;
        return 0;
    }
    pg_abandon(conn);
    return -1;
}

/* Read one frame. The returned body is valid until the next call, which
 * compacts the previously-consumed bytes. Returns 0 / -1. */
static int conn_next_frame(HlPgConn *conn, HlPgFrame *f)
{
    uint64_t started = 0;   /* this frame's reads share one ?read_timeout= */
    int got = 0;            /* a chunk of it came in */
    pg_rbuf_compact(conn);
    for (;;) {
        size_t consumed = 0;
        HlPgResult r = hl_pg_frame_next(conn->rbuf, conn->rlen, f, &consumed);
        if (r == HL_PG_OK) { conn->consumed = consumed; return 0; }
        if (r == HL_PG_ERR) {
            conn->broken = 1;
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "malformed message from server");
            return -1;
        }
        /* Still incomplete after a chunk: the run is asked after every one,
         * not only after a quiet slice - a server sending a large frame a
         * byte at a time, each within a slice, was never stopped (audit 21). */
        if (got && !conn->finishing && pg_wait_stop(NULL) &&
            pg_stop_or_finish(conn) != 0)
            return -1;
        /* NEED_MORE: grow if the buffer is full (bounded), then read. */
        if (conn->rlen == conn->rcap) {
            size_t ncap = conn->rcap ? conn->rcap * 2 : HL_PG_RBUF_INITIAL;
            /* frame_next rejects any length > HL_PG_MAX_MSG, so a legitimate
             * frame never needs more than that plus its 5-byte header. */
            if (ncap > (size_t)HL_PG_MAX_MSG + 8192) {
                conn->broken = 1;
                set_err(conn->errmsg, sizeof conn->errmsg,
                        "server message exceeds limit");
                return -1;
            }
            uint8_t *nb = realloc(conn->rbuf, ncap);
            if (!nb) { conn->broken = 1;
                       set_err(conn->errmsg, sizeof conn->errmsg, "out of memory");
                       return -1; }
            conn->rbuf = nb;
            conn->rcap = ncap;
        }
        /* Wait for the server in slices, asking the run's budget between
         * them: a recv blocked for the whole ?read_timeout= (300 s by
         * default, 0 = for ever) while the server ran pg_sleep or waited on a
         * lock, with no budget and no run deadline checked (audit 18). A trip
         * closes the connection, as a trip mid-reply does. */
        int left = hl_db_transport_time_left(&started, conn->read_timeout_ms);
        if (left < 0) {
            conn->broken = 1;
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "read failed or timed out (see ?read_timeout=)");
            return -1;
        }
        /* Finishing a tripped run's COMMIT reply asks no budget, so its whole
         * read is held to a short absolute bound instead (audit 26): a server
         * that stalled between CommandComplete and ReadyForQuery held the
         * loop for ?read_timeout= (300 s; for ever with 0). On expiry the
         * connection is abandoned, tx_status still 'T': refuse until ROLLBACK. */
        if (conn->finishing) {
            int fleft = pg_finish_left(conn);
            if (fleft < 0) { pg_abandon(conn); return -1; }
            if (left == 0 || fleft < left) left = fleft;
        }
        int wr = hl_db_transport_wait_readable(conn->transport, left,
                                               conn->finishing ? NULL : pg_wait_stop,
                                               NULL);
        if (wr == -2) {
            if (pg_stop_or_finish(conn) != 0) return -1;
            continue;   /* finishing now: wait again, under its bound */
        }
        /* What the wait left: passed its own `left`, the read could spend as
         * long again (audit 22). While finishing that is the finishing bound
         * too: recomputed from ?read_timeout= alone, the TLS read below ran
         * with no stop and, with read_timeout=0, no bound (audit 27). */
        if (wr == 1 && (left = hl_pg_conn_io_left(conn, &started)) < 0)
            wr = 0;
        /* Over TLS, sliced too: a record's first byte made it readable, and
         * the read then blocked for the rest under SO_RCVTIMEO (audit 20). */
        ssize_t n = wr == 1
            ? hl_db_transport_recv_sliced(conn->transport, conn->rbuf + conn->rlen,
                                          conn->rcap - conn->rlen, left,
                                          conn->finishing ? NULL : pg_wait_stop,
                                          NULL)
            : -1;
        if (n == -2) { pg_abandon(conn); return -1; }
        if (n < 0 && conn->finishing) { pg_abandon(conn); return -1; }
        if (n <= 0) {
            conn->broken = 1;
            set_err(conn->errmsg, sizeof conn->errmsg,
                    n < 0 ? "read failed or timed out (see ?read_timeout=)"
                          : "connection closed by server");
            return -1;
        }
        conn->rlen += (size_t)n;
        got = 1;
    }
}

/* ── Handshake ────────────────────────────────────────────────────── */

static int pg_abandon_if_tripped(HlPgConn *conn, uint32_t body_len);

static void conn_teardown(HlPgConn *conn)
{
    /* Best-effort: the transport frees any attached TLS session and closes the
     * descriptor. A -1 (a live connect op that will not confirm detachment) is an
     * accepted rare intentional leak, the same contract as the transport. A
     * broken connection is aborted: no TLS close_notify into a stream that is
     * out of step, which blocked on a peer that had stopped reading (audit 24). */
    if (conn->transport) {
        if (conn->broken)
            hl_db_transport_abort(conn->transport);
        else
            hl_db_transport_close(conn->transport);
        conn->transport = NULL;
    }
    free(conn->rbuf);
    conn->rbuf = NULL;
    conn->rcap = conn->rlen = conn->consumed = 0;
}

/* ── SCRAM-SHA-256 SASL exchange (RFC 5802 / RFC 7677) ────────────── */

#ifndef HL_PG_NO_SCRAM
typedef struct {
    char    client_nonce[48];        /* base64 of 24 random bytes (32 chars) */
    char    client_first_bare[128];  /* "n=,r=<nonce>" */
    uint8_t server_sig[32];          /* expected ServerSignature */
    int     have_server_sig;
    int     started;                 /* the server asked for SASL */
    int     verified;                /* its SASLFinal signature checked out */
} PgScram;

/* Handle one SASL AuthenticationRequest sub-message. Returns 0 to continue
 * the handshake, -1 on failure (conn->errmsg set). */
static int scram_handle(HlPgConn *conn, PgScram *sc, const HlPgDsn *dsn,
                        int32_t sub, HlPgCursor *c, int weak_iters_ok)
{
    if (sub == HL_PG_AUTH_SASL) {
        sc->started = 1;
        int have = 0;
        for (;;) {
            const char *m = hl_pg_get_cstr(c);
            if (!m || m[0] == '\0') break;
            if (strcmp(m, "SCRAM-SHA-256") == 0) have = 1;
        }
        if (!have) {
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "server offers no supported SASL mechanism (need SCRAM-SHA-256)");
            return -1;
        }
        uint8_t rnd[24];
        if (hl_cap_crypto_random(rnd, sizeof rnd) != 0 ||
            hl_base64_encode(rnd, sizeof rnd,
                             sc->client_nonce, sizeof sc->client_nonce, 0) < 0) {
            set_err(conn->errmsg, sizeof conn->errmsg, "SCRAM nonce generation failed");
            return -1;
        }
        /* Empty SCRAM username: PostgreSQL uses the startup message's user. */
        int n = snprintf(sc->client_first_bare, sizeof sc->client_first_bare,
                         "n=,r=%s", sc->client_nonce);
        char cfirst[192];
        int cf = snprintf(cfirst, sizeof cfirst, "n,,%s", sc->client_first_bare);
        if (n < 0 || (size_t)n >= sizeof sc->client_first_bare ||
            cf < 0 || (size_t)cf >= sizeof cfirst) {
            set_err(conn->errmsg, sizeof conn->errmsg, "SCRAM message too long");
            return -1;
        }
        HlPgWriter w;
        hl_pg_writer_init(&w);
        size_t m = hl_pg_msg_begin(&w, HL_PG_F_PASSWORD);   /* 'p' */
        hl_pg_put_cstr(&w, "SCRAM-SHA-256");
        hl_pg_put_i32(&w, cf);
        hl_pg_put_bytes(&w, cfirst, (size_t)cf);
        hl_pg_msg_end(&w, m);
        int se = w.err || conn_send(conn, w.buf, w.len);
        hl_pg_writer_free(&w);
        if (se) { set_err(conn->errmsg, sizeof conn->errmsg,
                          "failed to send SASL initial response"); return -1; }
        return 0;
    }

    if (sub == HL_PG_AUTH_SASL_CONTINUE) {
        size_t rem = c->remaining;
        const uint8_t *sfp = hl_pg_get_bytes(c, rem);
        if (!sfp || rem == 0 || rem >= 512) {
            set_err(conn->errmsg, sizeof conn->errmsg, "malformed SASL continue");
            return -1;
        }
        char server_first[512];
        memcpy(server_first, sfp, rem);
        server_first[rem] = '\0';

        char rnonce[160] = {0}, salt_b64[160] = {0};
        int iters = 0;
        for (const char *p = server_first; *p;) {
            const char *comma = strchr(p, ',');
            size_t flen = comma ? (size_t)(comma - p) : strlen(p);
            if (flen >= 2 && p[1] == '=') {
                const char *val = p + 2;
                size_t vlen = flen - 2;
                if (p[0] == 'r' && vlen < sizeof rnonce) {
                    memcpy(rnonce, val, vlen); rnonce[vlen] = '\0';
                } else if (p[0] == 's' && vlen < sizeof salt_b64) {
                    memcpy(salt_b64, val, vlen); salt_b64[vlen] = '\0';
                } else if (p[0] == 'i') {
                    /* The iteration count comes from the SERVER and drives the
                     * PBKDF2 loop below, so it is bounded on BOTH sides.
                     *
                     * atoi() cannot do that: it reports no error, and this
                     * field holds up to 15 digits, so a value above INT_MAX is
                     * undefined behaviour rather than a rejection. Bounded only
                     * below (iters <= 0), a peer answering i=2000000000 made
                     * the client run two billion HMAC-SHA256 rounds - a hang,
                     * from a peer that only has to be reachable: sslmode
                     * defaults to `prefer`, so a MITM can downgrade and supply
                     * it.
                     *
                     * SCRAM_MAX_ITERS is far above anything a real server
                     * asks for (RFC 5802 recommends at least 4096; Postgres
                     * ships 4096) and far below a stall. */
                    char ib[16];
                    if (vlen > 0 && vlen < sizeof ib) {
                        memcpy(ib, val, vlen); ib[vlen] = '\0';
                        int digits = 1;
                        for (size_t k = 0; k < vlen; k++)
                            if (!isdigit((unsigned char)ib[k])) { digits = 0; break; }
                        if (digits) {
                            errno = 0;
                            char *iend = NULL;
                            long v = strtol(ib, &iend, 10);
                            if (errno == 0 && iend && *iend == '\0' &&
                                v >= 1 && v <= SCRAM_MAX_ITERS)
                                iters = (int)v;
                        }
                    }
                }
            }
            if (!comma) break;
            p = comma + 1;
        }
        if (!rnonce[0] || !salt_b64[0] || iters <= 0) {
            /* iters is 0 both when absent and when it failed validation above
             * (non-numeric, out of range, or past SCRAM_MAX_ITERS); naming the
             * bound is what makes an otherwise baffling rejection diagnosable
             * against a server that genuinely wants a huge count. */
            set_err(conn->errmsg, sizeof conn->errmsg,
                    iters <= 0 && rnonce[0] && salt_b64[0]
                        ? "SCRAM iteration count missing or outside 1.."
                          HL_STR(SCRAM_MAX_ITERS)
                        : "invalid SCRAM server-first");
            return -1;
        }
        if (iters < SCRAM_MIN_ITERS && !weak_iters_ok) {
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "SCRAM iteration count below " HL_STR(SCRAM_MIN_ITERS)
                    " refused over an unverified connection (use "
                    "sslmode=verify-full)");
            return -1;
        }
        /* The iteration count is the server's: up to SCRAM_MAX_ITERS rounds
         * of HMAC run inside this one call, so they are charged to the run
         * as soon as the count is accepted, and a run that cannot pay does
         * not start them (audit 17). */
        if (hl_db_budget_charge((int64_t)iters * HL_PG_SCRAM_UNITS_PER_ITER) != 0) {
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "interrupted (instruction limit exceeded) before the "
                    "SCRAM key derivation");
            return -1;
        }
        /* The server's nonce must EXTEND ours (RFC 5802 5.1): one equal to
         * the client's added no server randomness, and a prefix check alone
         * took it (audit 29). */
        size_t cn_len = strlen(sc->client_nonce);
        if (strlen(rnonce) <= cn_len ||
            strncmp(rnonce, sc->client_nonce, cn_len) != 0) {
            set_err(conn->errmsg, sizeof conn->errmsg, "SCRAM nonce mismatch");
            return -1;
        }
        uint8_t salt[128];
        size_t salt_len = 0;
        if (hl_base64_decode(salt_b64, strlen(salt_b64), salt, sizeof salt, &salt_len, 0) != 0) {
            set_err(conn->errmsg, sizeof conn->errmsg, "bad SCRAM salt");
            return -1;
        }
        char cfinal_bare[192];
        int cfb = snprintf(cfinal_bare, sizeof cfinal_bare, "c=biws,r=%s", rnonce);
        char authmsg[1200];
        int am = snprintf(authmsg, sizeof authmsg, "%s,%s,%s",
                          sc->client_first_bare, server_first, cfinal_bare);
        if (cfb < 0 || (size_t)cfb >= sizeof cfinal_bare ||
            am < 0 || (size_t)am >= sizeof authmsg) {
            set_err(conn->errmsg, sizeof conn->errmsg, "SCRAM auth message too long");
            return -1;
        }
        uint8_t proof[32];
        if (hl_pg_scram_proof(dsn->password, strlen(dsn->password),
                              salt, salt_len, iters, authmsg, (size_t)am,
                              proof, sc->server_sig) != 0) {
            set_err(conn->errmsg, sizeof conn->errmsg, "SCRAM proof computation failed");
            return -1;
        }
        sc->have_server_sig = 1;

        char proof_b64[64], cfinal[288];
        if (hl_base64_encode(proof, 32, proof_b64, sizeof proof_b64, 0) < 0) return -1;
        int cf = snprintf(cfinal, sizeof cfinal, "%s,p=%s", cfinal_bare, proof_b64);
        if (cf < 0 || (size_t)cf >= sizeof cfinal) {
            set_err(conn->errmsg, sizeof conn->errmsg, "SCRAM final too long");
            return -1;
        }
        HlPgWriter w;
        hl_pg_writer_init(&w);
        size_t mm = hl_pg_msg_begin(&w, HL_PG_F_PASSWORD);
        hl_pg_put_bytes(&w, cfinal, (size_t)cf);
        hl_pg_msg_end(&w, mm);
        int se = w.err || conn_send(conn, w.buf, w.len);
        hl_pg_writer_free(&w);
        if (se) { set_err(conn->errmsg, sizeof conn->errmsg,
                          "failed to send SASL response"); return -1; }
        return 0;
    }

    if (sub == HL_PG_AUTH_SASL_FINAL) {
        size_t rem = c->remaining;
        const uint8_t *sfp = hl_pg_get_bytes(c, rem);
        if (!sfp || rem < 2 || sfp[0] != 'v' || sfp[1] != '=' || !sc->have_server_sig) {
            set_err(conn->errmsg, sizeof conn->errmsg, "malformed SASL final");
            return -1;
        }
        uint8_t got[32];
        size_t gl = 0;
        if (hl_base64_decode((const char *)sfp + 2, rem - 2, got, sizeof got, &gl, 0) != 0
            || gl != 32) {
            set_err(conn->errmsg, sizeof conn->errmsg, "bad SCRAM server signature");
            return -1;
        }
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= (uint8_t)(got[i] ^ sc->server_sig[i]);
        if (diff) {
            set_err(conn->errmsg, sizeof conn->errmsg, "SCRAM server signature mismatch");
            return -1;
        }
        sc->verified = 1;
        return 0;   /* server sends AuthenticationOk next */
    }

    set_err(conn->errmsg, sizeof conn->errmsg, "unsupported SASL sub-message");
    return -1;
}
#endif /* HL_PG_NO_SCRAM */

/* Run the startup + auth handshake over @p transport, which conn takes ownership
 * of (freed by conn_teardown). When TLS was negotiated the caller has already
 * attached the session to @p transport, so every byte tunnels through it
 * transparently. SIGPIPE suppression is the transport's job (adopt / connect set
 * SO_NOSIGPIPE on the descriptor). */
/* `tls_verified`: the transport carries a TLS session whose certificate chain
 * and host name were verified (sslmode=verify-ca / verify-full). Cleartext auth
 * is only sent over one, unless the DSN said sslmode=disable (see
 * hl_pg_cleartext_allowed). */
static int pg_start(HlPgConn *conn, HlDbTransport *transport, const HlPgDsn *dsn,
                    int tls_verified)
{
    memset(conn, 0, sizeof(*conn));
    conn->transport = transport;
    conn->read_timeout_ms = dsn->read_timeout_ms;

    HlPgWriter w;
    hl_pg_writer_init(&w);
    hl_pg_build_startup(&w, dsn->user, dsn->dbname);
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_pg_writer_free(&w);
    if (se) {
        if (!conn->errmsg[0])
            set_err(conn->errmsg, sizeof conn->errmsg, "failed to send startup");
        conn_teardown(conn);
        return -1;
    }

    int authenticated = 0;
    int sent_password = 0;   /* answered a cleartext request */
    int guard = 0;
#ifndef HL_PG_NO_SCRAM
    PgScram scram;
    memset(&scram, 0, sizeof scram);
#endif
    for (;;) {
        if (++guard > 1000) {
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "too many messages during handshake");
            conn_teardown(conn);
            return -1;
        }

        HlPgFrame f;
        if (conn_next_frame(conn, &f) != 0) { conn_teardown(conn); return -1; }
        /* The startup exchange is read for the run that opens the connection
         * (a first query, a reconnect), so its frames are charged like a
         * reply's: up to 1000 ParameterStatus / Notice frames of any size were
         * read uncharged (audit 17). */
        if (pg_abandon_if_tripped(conn, f.body_len)) return -1;

        HlPgCursor c;
        hl_pg_cursor_init(&c, &f);

        switch (f.type) {
        case HL_PG_B_AUTH: {
            int32_t sub = hl_pg_get_i32(&c);
            if (hl_pg_cursor_err(&c)) {
                set_err(conn->errmsg, sizeof conn->errmsg, "malformed auth request");
                conn_teardown(conn); return -1;
            }
            if (sub == HL_PG_AUTH_OK) {
#ifndef HL_PG_NO_SCRAM
                /* Once SCRAM has begun, only a verified server signature
                 * proves the server knows the password. Taking OK without it
                 * let anyone in the middle (sslmode=require checks no
                 * certificate) skip the SASLFinal and be trusted. */
                if (scram.started && !scram.verified) {
                    set_err(conn->errmsg, sizeof conn->errmsg,
                            "server ended SCRAM without proving its signature");
                    conn_teardown(conn); return -1;
                }
#endif
                /* With a password in the DSN the server must ask for it -
                 * by SCRAM, which also proves the server knows it, or in
                 * cleartext. An OK with no exchange at all ("trust") is what
                 * anyone in the middle answers, and sslmode=require checks
                 * no certificate. */
                int proven = sent_password;
#ifndef HL_PG_NO_SCRAM
                proven = proven || scram.verified;
#endif
                if (dsn->password[0] && !proven) {
                    set_err(conn->errmsg, sizeof conn->errmsg,
                            "server accepted the connection without asking for "
                            "the password; refused (drop the password from the "
                            "DSN if the server really trusts this client)");
                    conn_teardown(conn); return -1;
                }
                authenticated = 1;
            } else if (sub == HL_PG_AUTH_CLEARTEXT) {
                /* A cleartext request hands over the password itself. Refuse it
                 * mid-SCRAM (a downgrade), and unless the session is verified
                 * TLS or the DSN chose plaintext (sslmode=disable): see
                 * hl_pg_cleartext_allowed. */
                int allowed = hl_pg_cleartext_allowed(
                    tls_verified, hl_pg_sslmode_parse(dsn->sslmode));
#ifndef HL_PG_NO_SCRAM
                if (scram.started) allowed = 0;
#endif
                if (!allowed) {
                    set_err(conn->errmsg, sizeof conn->errmsg,
                            "server asked for the password in cleartext over a "
                            "connection whose server identity was not verified; "
                            "refused (use sslmode=verify-full or verify-ca, a "
                            "SCRAM-SHA-256 server, or sslmode=disable on a "
                            "trusted network)");
                    conn_teardown(conn); return -1;
                }
                HlPgWriter pw;
                hl_pg_writer_init(&pw);
                hl_pg_build_password(&pw, dsn->password);
                int pe = pw.err || conn_send(conn, pw.buf, pw.len);
                hl_pg_writer_free(&pw);
                if (pe) {
                    if (!conn->errmsg[0])
                        set_err(conn->errmsg, sizeof conn->errmsg,
                                "failed to send password");
                    conn_teardown(conn); return -1;
                }
                sent_password = 1;
            } else if (sub == HL_PG_AUTH_SASL ||
                       sub == HL_PG_AUTH_SASL_CONTINUE ||
                       sub == HL_PG_AUTH_SASL_FINAL) {
#ifndef HL_PG_NO_SCRAM
                if (scram_handle(conn, &scram, dsn, sub, &c,
                                 hl_pg_cleartext_allowed(
                                     tls_verified,
                                     hl_pg_sslmode_parse(dsn->sslmode))) != 0) {
                    conn_teardown(conn); return -1;
                }
#else
                set_err(conn->errmsg, sizeof conn->errmsg,
                        "SCRAM-SHA-256 not available in this build");
                conn_teardown(conn); return -1;
#endif
            } else if (sub == HL_PG_AUTH_MD5) {
                set_err(conn->errmsg, sizeof conn->errmsg,
                        "md5 auth is not supported; use scram-sha-256 or trust");
                conn_teardown(conn); return -1;
            } else {
                snprintf(conn->errmsg, sizeof conn->errmsg,
                         "unsupported auth method %d", sub);
                conn_teardown(conn); return -1;
            }
            break;
        }
        case HL_PG_B_BACKEND_KEY:
            conn->backend_pid = hl_pg_get_i32(&c);
            conn->backend_key = hl_pg_get_i32(&c);
            break;
        case HL_PG_B_PARAM_STATUS:
        case HL_PG_B_NOTICE:
            break;   /* server GUC reports and notices: ignored for now */
        case HL_PG_B_READY:
            conn->tx_status = hl_pg_get_u8(&c);
            if (!authenticated) {
                set_err(conn->errmsg, sizeof conn->errmsg,
                        "server ready before authentication completed");
                conn_teardown(conn); return -1;
            }
            return 0;   /* handshake complete, connection idle */
        case HL_PG_B_ERROR: {
            char msg[200] = {0};
            for (;;) {
                uint8_t ftype = hl_pg_get_u8(&c);
                if (hl_pg_cursor_err(&c) || ftype == 0) break;
                const char *val = hl_pg_get_cstr(&c);
                if (!val) break;
                if (ftype == 'M') snprintf(msg, sizeof msg, "%s", val);
            }
            snprintf(conn->errmsg, sizeof conn->errmsg,
                     "server rejected connection: %s",
                     msg[0] ? msg : "(no message)");
            conn_teardown(conn); return -1;
        }
        default:
            break;   /* unknown pre-ready message: ignore, bounded by guard */
        }
    }
}

int hl_pg_conn_start(HlPgConn *conn, int fd, const HlPgDsn *dsn)
{
    /* Adopt takes ownership of fd on success; on failure the caller still owns
     * fd (adopt contract), so do NOT close it here. */
    HlDbTransport *t = hl_db_transport_adopt("pg", NULL, fd, NULL, conn->errmsg, sizeof conn->errmsg);
    if (!t) {
        char saved[sizeof conn->errmsg];
        snprintf(saved, sizeof saved, "%s", conn->errmsg);
        memset(conn, 0, sizeof(*conn));
        snprintf(conn->errmsg, sizeof conn->errmsg, "%s", saved);
        return -1;
    }
    /* On any handshake failure pg_start's error path runs conn_teardown, which
     * closes the transport (and thus the adopted fd), matching the documented
     * "closed on handshake failure" behavior without a double close. */
    return pg_start(conn, t, dsn, 0);
}

#endif /* HL_PG_NO_TLS: end of the first connection-layer region */

/* ── TLS negotiation (SSLRequest / sslmode) ───────────────────────── */

/* Pure: maps a DSN sslmode string to the enum. Stays outside the transport guard
 * so the pure-parser fuzzers reach it. */
int hl_pg_cleartext_allowed(int tls_verified, int sslmode)
{
    return tls_verified || sslmode == HL_PG_SSLMODE_DISABLE;
}

int hl_pg_sslmode_parse(const char *s)
{
    if (!s || !s[0])                   return HL_PG_SSLMODE_PREFER;
    if (strcmp(s, "disable") == 0)     return HL_PG_SSLMODE_DISABLE;
    if (strcmp(s, "prefer") == 0)      return HL_PG_SSLMODE_PREFER;
    if (strcmp(s, "require") == 0)     return HL_PG_SSLMODE_REQUIRE;
    if (strcmp(s, "verify-ca") == 0 ||
        strcmp(s, "verify-full") == 0) return HL_PG_SSLMODE_VERIFY;
    return -1;
}

#ifndef HL_PG_NO_TLS

HlPgSslDecision hl_pg_ssl_negotiate(HlDbTransport *t, HlPgSslMode mode,
                                    int read_timeout_ms,
                                    char *errbuf, size_t errlen)
{
    if (mode == HL_PG_SSLMODE_DISABLE) return HL_PG_SSL_PLAINTEXT;

    /* SSLRequest: length(8) then request code 80877103 (0x04d2162f). This pre-TLS
     * plaintext probe rides the transport (no TLS attached yet), so send_all / recv
     * go through the provider - the same production I/O path as the rest of the
     * protocol, not a separate raw send/recv. */
    static const uint8_t req[8] = { 0, 0, 0, 8, 0x04, 0xd2, 0x16, 0x2f };
    /* Sliced like every later write (audit 23). */
    int sr = hl_db_transport_send_sliced(t, req, sizeof req, read_timeout_ms,
                                         pg_wait_stop, NULL);
    if (sr != 0) {
        set_err(errbuf, errlen, sr == -2
                ? "connect stopped (instruction or time limit exceeded)"
                : "failed to send SSLRequest");
        return HL_PG_SSL_FAIL;
    }

    /* Waited for in slices that ask the run's budget, as every later read
     * is: a server that accepted and never answered the SSLRequest (sent by
     * default, sslmode=prefer) held the event loop for the whole read
     * timeout - for ever under ?read_timeout=0 (audit 20). */
    uint8_t resp;
    int wr = hl_db_transport_wait_readable(t, read_timeout_ms, pg_wait_stop, NULL);
    if (wr == -2) { set_err(errbuf, errlen,
                            "connect stopped (instruction or time limit exceeded)");
                    return HL_PG_SSL_FAIL; }
    ssize_t n = wr == 1 ? hl_db_transport_recv(t, &resp, 1) : -1;
    if (n <= 0) { set_err(errbuf, errlen, "no SSLRequest response from server");
                  return HL_PG_SSL_FAIL; }

    if (resp == 'S') return HL_PG_SSL_USE_TLS;
    if (resp == 'N') {
        if (mode >= HL_PG_SSLMODE_REQUIRE) {
            set_err(errbuf, errlen,
                    "server does not support TLS but sslmode requires it");
            return HL_PG_SSL_FAIL;
        }
        return HL_PG_SSL_PLAINTEXT;
    }
    set_err(errbuf, errlen, "unexpected SSLRequest response from server");
    return HL_PG_SSL_FAIL;
}

int hl_pg_conn_open(HlPgConn *conn, const HlPgDsn *dsn, int timeout_ms)
{
    HlDbTransport *t = hl_db_transport_connect_stop("pg", NULL, dsn->host, dsn->port,
                                                  timeout_ms, NULL, pg_wait_stop, NULL,
                                                  conn->errmsg, sizeof conn->errmsg);
    if (!t) {
        memset(conn, 0, sizeof(*conn));
        conn->transport = NULL;
        /* Overwrite the transport's message with the historical public token. */
        snprintf(conn->errmsg, sizeof conn->errmsg,
                 "cannot connect to %s:%s", dsn->host, dsn->port);
        return -1;
    }
    /* Bound every read from here on - the SSLRequest answer, the startup and
     * auth exchange, every query. Only the connect and the TLS handshake had a
     * deadline: a server that accepted and then stalled blocked forever. */
    hl_db_transport_set_io_timeout(t, dsn->read_timeout_ms);

    int mode = hl_pg_sslmode_parse(dsn->sslmode);
    if (mode < 0) {
        memset(conn, 0, sizeof(*conn));
        conn->transport = NULL;
        snprintf(conn->errmsg, sizeof conn->errmsg,
                 "unknown sslmode: %s", dsn->sslmode);
        hl_db_transport_close(t);
        return -1;
    }
    if (mode != HL_PG_SSLMODE_DISABLE) {
        /* SSLRequest pre-TLS probe rides the transport (plaintext, no TLS yet). */
        char e[128] = {0};
        HlPgSslDecision d = hl_pg_ssl_negotiate(t, (HlPgSslMode)mode,
                                                dsn->read_timeout_ms, e, sizeof e);
        if (d == HL_PG_SSL_FAIL) {
            memset(conn, 0, sizeof(*conn));
            conn->transport = NULL;
            snprintf(conn->errmsg, sizeof conn->errmsg, "%s", e);
            hl_db_transport_close(t);
            return -1;
        }
        if (d == HL_PG_SSL_USE_TLS) {
            /* verify-ca / verify-full check the chain + hostname against the
             * active trust anchor (hl_ca_bundle_active: --ca-bundle, the system
             * store, or the embedded bundle); require / prefer take the session
             * as-is. The
             * handshake needs the raw descriptor (hl_tls_client_handshake takes an
             * int fd); the resulting session is then handed to the transport so
             * subsequent bytes tunnel through it. */
            int fd = hl_db_transport_fd(t);
            int verify = (mode == HL_PG_SSLMODE_VERIFY);
            struct HlTlsClient *tls =
                hl_tls_client_handshake_stop(fd, dsn->host, verify, timeout_ms,
                                             pg_wait_stop, NULL);
            if (!tls) {
                memset(conn, 0, sizeof(*conn));
                conn->transport = NULL;
                snprintf(conn->errmsg, sizeof conn->errmsg,
                         "TLS handshake with %s failed", dsn->host);
                hl_db_transport_close(t);
                return -1;
            }
            if (hl_db_transport_attach_tls(t, tls) != 0) {
                hl_tls_client_free(tls);
                memset(conn, 0, sizeof(*conn));
                conn->transport = NULL;
                snprintf(conn->errmsg, sizeof conn->errmsg,
                         "TLS handshake with %s failed", dsn->host);
                hl_db_transport_close(t);
                return -1;
            }
            return pg_start(conn, t, dsn, verify);
        }
        /* PLAINTEXT: server declined TLS and sslmode permits fallback. */
    }

    return pg_start(conn, t, dsn, 0);
}

void hl_pg_conn_close(HlPgConn *conn)
{
    if (!conn) return;
    /* Best effort, and never a wait: a blocking Terminate on a socket the
     * server had stopped reading waited out SO_SNDTIMEO (for ever with
     * ?read_timeout=0), and on a broken connection it would land in the
     * middle of a part-sent message anyway (audit 23). */
    if (conn->transport && !conn->broken) {
        HlPgWriter w;
        hl_pg_writer_init(&w);
        hl_pg_build_terminate(&w);
        if (!w.err)
            (void)hl_db_transport_send_sliced(conn->transport, w.buf, w.len,
                                              1, NULL, NULL);
        hl_pg_writer_free(&w);
    }
    conn_teardown(conn);   /* closes + frees the transport, idempotent */
}

#endif /* HL_PG_NO_TLS: end of the second connection-layer region */

/* ── SCRAM-SHA-256 ──────────────────────────────────────────────── */

#ifndef HL_PG_NO_SCRAM
/*
 * PBKDF2-HMAC-SHA256 with dkLen = 32 (one output block). SCRAM uses the
 * server-chosen iteration count (commonly 4096), which is below the
 * password-hashing floor cap/crypto's hl_cap_crypto_pbkdf2 enforces, so this
 * iterates HMAC here. It still goes through the cap HMAC primitive, so it
 * inherits the backend + never calls mbedTLS directly.
 */
static int scram_pbkdf2(const char *pw, size_t pw_len,
                        const uint8_t *salt, size_t salt_len, int iters,
                        uint8_t out[32])
{
    if (iters < 1 || salt_len > 4096) return -1;
    uint8_t *msg = malloc(salt_len + 4);
    if (!msg) return -1;
    memcpy(msg, salt, salt_len);
    msg[salt_len + 0] = 0; msg[salt_len + 1] = 0;
    msg[salt_len + 2] = 0; msg[salt_len + 3] = 1;   /* INT(1) */

    /* u / t / next are the PBKDF2 state, from which the salted password
     * follows: scrubbed on every return, the failures included (audit 29). */
    uint8_t u[32], t[32], next[32];
    int ret = -1;
    int rc = hl_cap_crypto_hmac_sha256((const uint8_t *)pw, pw_len,
                                       msg, salt_len + 4, u);
    free(msg);
    if (rc != 0) goto out;
    memcpy(t, u, 32);

    for (int i = 1; i < iters; i++) {
        /* HMAC into a separate buffer, not in-place over the input `u`: the
         * cap-layer HMAC backend is swappable (HL_HMAC_BACKEND), so don't
         * assume it buffers the message before writing `out`. */
        if (hl_cap_crypto_hmac_sha256((const uint8_t *)pw, pw_len, u, 32, next) != 0)
            goto out;
        memcpy(u, next, 32);
        for (int j = 0; j < 32; j++) t[j] = (uint8_t)(t[j] ^ u[j]);
    }
    memcpy(out, t, 32);
    ret = 0;
out:
    pg_secure_zero(u, sizeof u);
    pg_secure_zero(t, sizeof t);
    pg_secure_zero(next, sizeof next);
    return ret;
}

int hl_pg_scram_proof(const char *password, size_t pw_len,
                      const uint8_t *salt, size_t salt_len, int iterations,
                      const char *auth_msg, size_t auth_msg_len,
                      uint8_t client_proof[32], uint8_t server_sig[32])
{
    uint8_t salted[32], client_key[32], stored_key[32];
    uint8_t client_sig[32], server_key[32];
    int ret = -1;

    if (scram_pbkdf2(password, pw_len, salt, salt_len, iterations, salted) != 0)
        goto out;
    if (hl_cap_crypto_hmac_sha256(salted, 32,
            (const uint8_t *)"Client Key", 10, client_key) != 0) goto out;
    if (hl_cap_crypto_sha256(client_key, 32, stored_key) != 0) goto out;
    if (hl_cap_crypto_hmac_sha256(stored_key, 32,
            (const uint8_t *)auth_msg, auth_msg_len, client_sig) != 0) goto out;
    for (int i = 0; i < 32; i++)
        client_proof[i] = (uint8_t)(client_key[i] ^ client_sig[i]);

    if (hl_cap_crypto_hmac_sha256(salted, 32,
            (const uint8_t *)"Server Key", 10, server_key) != 0) goto out;
    if (hl_cap_crypto_hmac_sha256(server_key, 32,
            (const uint8_t *)auth_msg, auth_msg_len, server_sig) != 0) goto out;
    ret = 0;
out:
    /* Scrub the derived secrets on every path - stored_key and client_sig
     * too, and the early failures (audit 29). */
    pg_secure_zero(salted, sizeof salted);
    pg_secure_zero(client_key, sizeof client_key);
    pg_secure_zero(stored_key, sizeof stored_key);
    pg_secure_zero(client_sig, sizeof client_sig);
    pg_secure_zero(server_key, sizeof server_key);
    return ret;
}
#endif /* HL_PG_NO_SCRAM */

/* ── Placeholder rewriting ────────────────────────────────────────── */

int hl_pg_rewrite_sql(const char *sql, char *out, size_t outsize, int *nparams)
{
    if (nparams) *nparams = 0;
    if (!sql || !out || outsize == 0) return -1;

    size_t o = 0;
    int n = 0;
    const char *p = sql;

#define RW_PUT(ch) do { if (o + 1 >= outsize) return -1; out[o++] = (char)(ch); } while (0)

    while (*p) {
        char c = *p;

        /* Single-quoted string or double-quoted identifier; doubled quote
         * is an in-literal escape. A '?' inside is left untouched. */
        if (c == '\'' || c == '"') {
            char q = c;
            RW_PUT(c);
            p++;
            while (*p) {
                if (*p == q) {
                    if (p[1] == q) { RW_PUT(q); RW_PUT(q); p += 2; continue; }
                    RW_PUT(q); p++;
                    break;
                }
                RW_PUT(*p);
                p++;
            }
            continue;
        }

        /* Line comment. */
        if (c == '-' && p[1] == '-') {
            while (*p && *p != '\n') { RW_PUT(*p); p++; }
            continue;
        }

        /* Block comment (PostgreSQL allows nesting). */
        if (c == '/' && p[1] == '*') {
            RW_PUT('/'); RW_PUT('*'); p += 2;
            int depth = 1;
            while (*p && depth > 0) {
                if (p[0] == '/' && p[1] == '*') { RW_PUT('/'); RW_PUT('*'); p += 2; depth++; }
                else if (p[0] == '*' && p[1] == '/') { RW_PUT('*'); RW_PUT('/'); p += 2; depth--; }
                else { RW_PUT(*p); p++; }
            }
            continue;
        }

        /* Dollar-quoted string: $tag$ ... $tag$ (tag may be empty). */
        if (c == '$') {
            const char *q = p + 1;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
            if (*q == '$') {
                size_t dl = (size_t)(q - p) + 1;   /* length of "$tag$" */
                for (size_t i = 0; i < dl; i++) RW_PUT(p[i]);
                const char *scan = p + dl;
                while (*scan) {
                    if (*scan == '$' && strncmp(scan, p, dl) == 0) {
                        for (size_t i = 0; i < dl; i++) RW_PUT(scan[i]);
                        scan += dl;
                        break;
                    }
                    RW_PUT(*scan);
                    scan++;
                }
                p = scan;   /* unterminated: body already emitted verbatim */
                continue;
            }
            RW_PUT(c);
            p++;
            continue;
        }

        /* The placeholder. */
        if (c == '?') {
            char buf[16];
            int len = snprintf(buf, sizeof buf, "$%d", ++n);
            if (len < 0) return -1;
            for (int i = 0; i < len; i++) RW_PUT(buf[i]);
            p++;
            continue;
        }

        RW_PUT(c);
        p++;
    }

    if (o >= outsize) return -1;
    out[o] = '\0';
    if (nparams) *nparams = n;
    return 0;

#undef RW_PUT
}

/* ── Parameterized query execution ────────────────────────────────── */

/* The query + notify paths ride the transport (recv / send), so they sit inside
 * the same guard the connection layer uses; the pure-parser fuzzers omit them. */
#ifndef HL_PG_NO_TLS

/* Row count from a CommandComplete tag ("INSERT 0 5", "UPDATE 3", ...):
 * the last space-separated token when it is numeric, else -1. */
static int64_t parse_affected(const char *tag)
{
    const char *last = strrchr(tag, ' ');
    if (!last) return -1;
    last++;
    if (!*last) return -1;
    int64_t v = 0;
    for (const char *q = last; *q; q++) {
        if (*q < '0' || *q > '9') return -1;
        /* Overflow guard: a hostile CommandComplete tag must not trigger
         * signed-overflow UB (the count is only informational). */
        if (v > (INT64_MAX - 9) / 10) return -1;
        v = v * 10 + (*q - '0');
    }
    return v;
}

/* Column-count guard: PostgreSQL tops out at 1600 columns; cap generously. */
#define PG_MAX_COLS 4096

static int handle_row_desc(HlPgCursor *c, HlPgDescCb cb, void *ctx,
                           HlPgConn *conn)
{
    int nf = (int)hl_pg_get_i16(c);
    if (hl_pg_cursor_err(c) || nf < 0 || nf > PG_MAX_COLS) {
        set_err(conn->errmsg, sizeof conn->errmsg, "malformed RowDescription");
        return -1;
    }
    HlPgField *fields = nf ? calloc((size_t)nf, sizeof *fields) : NULL;
    if (nf && !fields) { set_err(conn->errmsg, sizeof conn->errmsg, "out of memory");
                         return -1; }
    for (int i = 0; i < nf; i++) {
        const char *name = hl_pg_get_cstr(c);
        (void)hl_pg_get_i32(c);   /* table oid   */
        (void)hl_pg_get_i16(c);   /* column attr */
        int32_t oid = hl_pg_get_i32(c);
        (void)hl_pg_get_i16(c);   /* type length */
        (void)hl_pg_get_i32(c);   /* type modifier */
        (void)hl_pg_get_i16(c);   /* format code */
        fields[i].name = name;
        fields[i].oid = oid;
    }
    if (hl_pg_cursor_err(c)) {
        free(fields);
        set_err(conn->errmsg, sizeof conn->errmsg, "malformed RowDescription");
        return -1;
    }
    if (cb) cb(ctx, fields, nf);
    free(fields);
    return 0;
}

/* Returns 0 to continue, 1 if the callback asked to stop, -1 on error. */
static int handle_data_row(HlPgCursor *c, HlPgRowCb cb, void *ctx,
                           HlPgConn *conn)
{
    int nc = (int)hl_pg_get_i16(c);
    if (hl_pg_cursor_err(c) || nc < 0 || nc > PG_MAX_COLS) {
        set_err(conn->errmsg, sizeof conn->errmsg, "malformed DataRow");
        return -1;
    }
    const char **vals = nc ? calloc((size_t)nc, sizeof *vals) : NULL;
    int32_t *lens = nc ? calloc((size_t)nc, sizeof *lens) : NULL;
    if (nc && (!vals || !lens)) {
        free(vals); free(lens);
        set_err(conn->errmsg, sizeof conn->errmsg, "out of memory");
        return -1;
    }
    for (int i = 0; i < nc; i++) {
        int32_t len = hl_pg_get_i32(c);
        if (len < 0) {
            vals[i] = NULL;
            lens[i] = -1;
        } else {
            const uint8_t *b = hl_pg_get_bytes(c, (size_t)len);
            vals[i] = (const char *)b;   /* NULL on underrun (cursor err) */
            lens[i] = len;
        }
    }
    if (hl_pg_cursor_err(c)) {
        free(vals); free(lens);
        set_err(conn->errmsg, sizeof conn->errmsg, "malformed DataRow");
        return -1;
    }
    int r = cb ? cb(ctx, vals, lens, nc) : 0;
    free(vals); free(lens);
    return r ? 1 : 0;
}

/* Each frame of a reply is charged to the run it is read for - 1 unit plus 1
 * per HL_DB_ALLOC_UNIT_BYTES of its body (cap/db_budget.h) - whatever its
 * type: a NoticeResponse stream (`LOOP RAISE NOTICE`) or a COPY stream costs
 * the server nothing to send, and a 0-unit charge only ASKED, so a db.async
 * op's budget (HlDbOpBudget, which a 0 charge never trips) bounded nothing
 * (audit 17). Once the run is over its instruction budget or past its
 * wall-clock deadline the binding raises that when the call returns, so the
 * rest of the reply would be read - at the server's pace, a result of any size - only to be thrown
 * away (audit 15). Drop the connection instead of draining it: the closed
 * socket stops the query at the server's next write, the connection is
 * broken, and the backend reconnects on the next call (db_postgres.c's
 * pg_ready) - or, inside a transaction, refuses everything but the ROLLBACK
 * the server has already done with the session. Simpler than a CancelRequest
 * on a side connection, and it needs no new connect from inside the run. */
static void pg_abandon(HlPgConn *conn)
{
    conn->broken = 1;      /* first: the teardown sends no close_notify */
    conn_teardown(conn);
    set_err(conn->errmsg, sizeof conn->errmsg,
            "interrupted (instruction limit exceeded); the connection was "
            "closed rather than read to the end of the reply");
}

static int64_t pg_frame_units(uint32_t body_len)
{
    return 1 + ((int64_t)body_len + HL_DB_ALLOC_UNIT_BYTES - 1) /
               HL_DB_ALLOC_UNIT_BYTES;
}

static int pg_abandon_if_tripped(HlPgConn *conn, uint32_t body_len)
{
    if (hl_db_budget_charge(pg_frame_units(body_len)) == 0) return 0;
    pg_abandon(conn);
    return 1;
}

/* Frames a reply may still carry after a tripped run's COMMIT completed
 * (a ParameterStatus, a Notice, the ReadyForQuery). */
#define HL_PG_FINISH_MAX_FRAMES 16

/* Charge one frame of a query's reply; 1 when the connection was abandoned.
 * A ReadyForQuery ends the reply, with nothing left to drop. A trip on the
 * CommandComplete of a COMMIT is not abandoned: the server has committed, and
 * a connection dropped there kept tx_status 'T', so the commit was reported
 * failed and then "rolled back" by the server (audit 18). The reply is read
 * on to its ReadyForQuery instead - without asking the budget again while
 * waiting for it, and for at most HL_PG_FINISH_MAX_FRAMES frames. The trip
 * is sticky, so the run raises it at its next poll once the call returns.
 * After a lone COMMIT was sent (conn->commit_sent) a trip on ANY frame of its
 * reply finishes the same way: the extended protocol's ParseComplete /
 * BindComplete / NoData come before the CommandComplete (audit 29). */
static int pg_reply_charge(HlPgConn *conn, const HlPgFrame *f)
{
    if (f->type == HL_PG_B_READY) return 0;
    if (conn->finishing) {
        if (++conn->finishing <= HL_PG_FINISH_MAX_FRAMES) return 0;
        conn->finishing = 0;
        pg_abandon(conn);
        return 1;
    }
    if (hl_db_budget_charge(pg_frame_units(f->body_len)) == 0) return 0;
    if (conn->commit_sent ||
        (f->type == HL_PG_B_CMD_COMPLETE && f->body_len >= 6 &&
         memcmp(f->body, "COMMIT", 6) == 0)) {
        conn->finishing = 1;
        conn->finish_started = 0;   /* the bound starts at the next read */
        return 0;
    }
    pg_abandon(conn);
    return 1;
}

/* The server answered a statement with CopyIn / CopyBoth: it now waits for
 * COPY data, and the client - reading for ReadyForQuery - waited for the
 * server: both sat until the read timeout. Hull does not stream COPY data,
 * so it answers CopyFail ('f'); the server then reports an error and goes
 * on to ReadyForQuery. -1 when even that could not be sent. */
static int pg_refuse_copy(HlPgConn *conn)
{
    HlPgWriter w;
    hl_pg_writer_init(&w);
    size_t m = hl_pg_msg_begin(&w, 'f');
    hl_pg_put_cstr(&w, "COPY FROM STDIN is not supported by hull/db");
    hl_pg_msg_end(&w, m);
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_pg_writer_free(&w);
    return se ? -1 : 0;
}

/* The server answered a statement with CopyOutResponse (`COPY ... TO
 * STDOUT`): CopyData follows at the server's pace, as much as the query
 * makes, and there is no CopyFail for this direction. Hull does not stream
 * COPY data, so the connection is dropped instead of read (audit 17): the
 * closed socket stops the COPY at the server's next write, and the backend
 * reconnects on the next call, as after a tripped run. Always -1. */
static int pg_refuse_copy_out(HlPgConn *conn)
{
    conn->broken = 1;      /* first: the teardown sends no close_notify */
    conn_teardown(conn);
    set_err(conn->errmsg, sizeof conn->errmsg,
            "COPY TO STDOUT is not supported by hull/db; the connection was "
            "closed rather than read to the end of the reply");
    return -1;
}

int hl_pg_query(HlPgConn *conn, const char *sql,
                const HlPgParam *params, int nparams,
                HlPgDescCb desc_cb, HlPgRowCb row_cb, void *cb_ctx,
                int64_t *affected)
{
    if (affected) *affected = -1;
    if (!conn || !conn->transport) {
        if (conn) set_err(conn->errmsg, sizeof conn->errmsg, "not connected");
        return -1;
    }
    if (conn->broken) {
        set_err(conn->errmsg, sizeof conn->errmsg,
                "connection to the server was lost");
        return -1;
    }
    conn->errmsg[0] = '\0';
    conn->finishing = 0;
    if (nparams < 0 || nparams > 65535) {
        set_err(conn->errmsg, sizeof conn->errmsg, "invalid parameter count");
        return -1;
    }

    /* Rewrite '?' -> '$n' into a buffer sized for the worst case: every '?'
     * in the text (quoted ones too - an upper bound) growing to "$65535".
     * It was sized from the VALUE count, so SQL with more '?' than values -
     * the trailing-nils case padded below - failed to rewrite. */
    size_t sql_len = strlen(sql), marks = 0;
    for (const char *q = sql; *q; q++)
        if (*q == '?') marks++;
    if (sql_len > SIZE_MAX / 8) {
        set_err(conn->errmsg, sizeof conn->errmsg, "query too long");
        return -1;
    }
    size_t rwcap = sql_len + marks * 5 + 32;
    char *rw = malloc(rwcap);
    if (!rw) { set_err(conn->errmsg, sizeof conn->errmsg, "out of memory"); return -1; }
    int found = 0;
    if (hl_pg_rewrite_sql(sql, rw, rwcap, &found) != 0) {
        free(rw);
        set_err(conn->errmsg, sizeof conn->errmsg, "failed to rewrite SQL placeholders");
        return -1;
    }
    /* The Bind message carries the count as an int16. */
    if (found > 65535) {
        free(rw);
        set_err(conn->errmsg, sizeof conn->errmsg, "too many placeholders (max 65535)");
        return -1;
    }
    /* Match SQLite's leniency: a Lua/JS params array with trailing nils
     * reports fewer values than there are placeholders (Lua's # stops at
     * the first nil), and SQLite binds the unbound tail as NULL. Pad the
     * missing trailing parameters here the same way. Only MORE values than
     * placeholders is a real error (SQLite raises SQLITE_RANGE too). */
    if (nparams > found) {
        snprintf(conn->errmsg, sizeof conn->errmsg,
                 "parameter count mismatch: %d placeholders, %d values",
                 found, nparams);
        free(rw);
        return -1;
    }

    /* Parse + Bind + Describe(portal) + Execute + Sync, one flush. */
    HlPgWriter w;
    hl_pg_writer_init(&w);

    size_t m = hl_pg_msg_begin(&w, HL_PG_F_PARSE);
    hl_pg_put_cstr(&w, "");            /* unnamed prepared statement */
    hl_pg_put_cstr(&w, rw);            /* the rewritten query */
    hl_pg_put_i16(&w, 0);              /* 0 parameter type oids: server infers */
    hl_pg_msg_end(&w, m);

    m = hl_pg_msg_begin(&w, HL_PG_F_BIND);
    hl_pg_put_cstr(&w, "");            /* unnamed portal */
    hl_pg_put_cstr(&w, "");            /* unnamed statement */
    hl_pg_put_i16(&w, 0);             /* 0 parameter format codes: all text */
    /* Bind `found` parameters to match $1..$found in the query; a missing
     * trailing param (i >= nparams) binds as SQL NULL, mirroring SQLite. */
    hl_pg_put_i16(&w, (int16_t)found);
    for (int i = 0; i < found; i++) {
        if (i >= nparams || !params[i].text) {
            hl_pg_put_i32(&w, -1);     /* SQL NULL */
        } else {
            int32_t len = params[i].len >= 0
                        ? params[i].len : (int32_t)strlen(params[i].text);
            hl_pg_put_i32(&w, len);
            hl_pg_put_bytes(&w, params[i].text, (size_t)len);
        }
    }
    hl_pg_put_i16(&w, 0);             /* 0 result format codes: all text */
    hl_pg_msg_end(&w, m);

    m = hl_pg_msg_begin(&w, HL_PG_F_DESCRIBE);
    hl_pg_put_u8(&w, 'P');             /* describe the portal */
    hl_pg_put_cstr(&w, "");
    hl_pg_msg_end(&w, m);

    m = hl_pg_msg_begin(&w, HL_PG_F_EXECUTE);
    hl_pg_put_cstr(&w, "");
    hl_pg_put_i32(&w, 0);              /* 0 = all rows */
    hl_pg_msg_end(&w, m);

    m = hl_pg_msg_begin(&w, HL_PG_F_SYNC);
    hl_pg_msg_end(&w, m);

    free(rw);
    conn->commit_sent = 0;
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_pg_writer_free(&w);
    if (se) {
        if (!conn->errmsg[0])
            set_err(conn->errmsg, sizeof conn->errmsg, "failed to send query");
        return -1;
    }
    conn->commit_sent = hl_sql_txn_kind_ex(sql, 1) == HL_SQL_TXN_COMMIT;

    /* Read the response stream until ReadyForQuery. */
    int had_error = 0;
    int stop_rows = 0;
    for (;;) {
        HlPgFrame f;
        if (conn_next_frame(conn, &f) != 0) return -1;

        /* Every frame but the last is charged (pg_reply_charge). */
        if (pg_reply_charge(conn, &f)) return -1;

        HlPgCursor c;
        hl_pg_cursor_init(&c, &f);

        switch (f.type) {
        case HL_PG_B_PARSE_COMPLETE:
        case HL_PG_B_BIND_COMPLETE:
        case HL_PG_B_NO_DATA:
        case HL_PG_B_EMPTY_QUERY:
        case HL_PG_B_PARAM_STATUS:
        case HL_PG_B_NOTICE:
            break;
        case HL_PG_B_ROW_DESC:
            if (handle_row_desc(&c, desc_cb, cb_ctx, conn) != 0) had_error = 1;
            break;
        case HL_PG_B_DATA_ROW:
            if (!stop_rows) {
                int r = handle_data_row(&c, row_cb, cb_ctx, conn);
                if (r < 0) had_error = 1;
                else if (r > 0) stop_rows = 1;
            }
            break;
        case HL_PG_B_CMD_COMPLETE: {
            const char *tag = hl_pg_get_cstr(&c);
            if (tag && affected) *affected = parse_affected(tag);
            break;
        }
        case HL_PG_B_ERROR: {
            char msg[200] = {0};
            for (;;) {
                uint8_t ft = hl_pg_get_u8(&c);
                if (hl_pg_cursor_err(&c) || ft == 0) break;
                const char *v = hl_pg_get_cstr(&c);
                if (!v) break;
                if (ft == 'M') snprintf(msg, sizeof msg, "%s", v);
            }
            snprintf(conn->errmsg, sizeof conn->errmsg,
                     "query failed: %s", msg[0] ? msg : "(no message)");
            had_error = 1;
            break;
        }
        case HL_PG_B_COPY_IN:
        case HL_PG_B_COPY_BOTH:
            if (pg_refuse_copy(conn) != 0) return -1;
            break;            /* the ErrorResponse that follows fails it */
        case HL_PG_B_COPY_OUT:
            return pg_refuse_copy_out(conn);
        case HL_PG_B_READY:
            conn->tx_status = hl_pg_get_u8(&c);
            conn->finishing = 0;
            conn->commit_sent = 0;
            return had_error ? -1 : 0;
        case HL_PG_B_NOTIFY:
            pg_note_notify(conn, &f);
            break;
        default:
            break;
        }
    }
}

int hl_pg_exec_simple(HlPgConn *conn, const char *sql)
{
    if (!conn || !conn->transport) {
        if (conn) set_err(conn->errmsg, sizeof conn->errmsg, "not connected");
        return -1;
    }
    if (conn->broken) {
        set_err(conn->errmsg, sizeof conn->errmsg,
                "connection to the server was lost");
        return -1;
    }
    conn->errmsg[0] = '\0';
    conn->finishing = 0;

    HlPgWriter w;
    hl_pg_writer_init(&w);
    size_t m = hl_pg_msg_begin(&w, HL_PG_F_QUERY);
    hl_pg_put_cstr(&w, sql);
    hl_pg_msg_end(&w, m);
    conn->commit_sent = 0;
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_pg_writer_free(&w);
    if (se) {
        if (!conn->errmsg[0])
            set_err(conn->errmsg, sizeof conn->errmsg, "failed to send script");
        return -1;
    }
    conn->commit_sent = hl_sql_txn_kind_ex(sql, 1) == HL_SQL_TXN_COMMIT;

    /* Simple Query returns a stream per statement; discard all results and
     * read to ReadyForQuery. A single ErrorResponse anywhere fails the whole
     * script (the server aborts the rest of the batch itself). */
    int had_error = 0;
    for (;;) {
        HlPgFrame f;
        if (conn_next_frame(conn, &f) != 0) return -1;

        if (pg_reply_charge(conn, &f)) return -1;

        HlPgCursor c;
        hl_pg_cursor_init(&c, &f);

        switch (f.type) {
        case HL_PG_B_DATA_ROW:
        case HL_PG_B_ROW_DESC:
        case HL_PG_B_CMD_COMPLETE:
        case HL_PG_B_EMPTY_QUERY:
        case HL_PG_B_PARAM_STATUS:
        case HL_PG_B_NOTICE:
            break;   /* results / notices discarded */
        case HL_PG_B_ERROR: {
            char msg[200] = {0};
            for (;;) {
                uint8_t ft = hl_pg_get_u8(&c);
                if (hl_pg_cursor_err(&c) || ft == 0) break;
                const char *v = hl_pg_get_cstr(&c);
                if (!v) break;
                if (ft == 'M') snprintf(msg, sizeof msg, "%s", v);
            }
            snprintf(conn->errmsg, sizeof conn->errmsg,
                     "script failed: %s", msg[0] ? msg : "(no message)");
            had_error = 1;
            break;
        }
        case HL_PG_B_COPY_IN:
        case HL_PG_B_COPY_BOTH:
            if (pg_refuse_copy(conn) != 0) return -1;
            break;            /* the ErrorResponse that follows fails it */
        case HL_PG_B_COPY_OUT:
            return pg_refuse_copy_out(conn);
        case HL_PG_B_READY:
            conn->tx_status = hl_pg_get_u8(&c);
            conn->finishing = 0;
            conn->commit_sent = 0;
            return had_error ? -1 : 0;
        case HL_PG_B_NOTIFY:
            pg_note_notify(conn, &f);
            break;
        default:
            break;
        }
    }
}

/* Monotonic milliseconds for the wait deadline (not wall-clock, so it is immune
 * to clock steps). */
static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 1 when the NotificationResponse @p f is for @p channel (NULL = any). The
 * server reports the folded name of the channel LISTEN registered; Hull's
 * channels are plain identifiers, so compare without case. A body that does
 * not parse matches nothing. */
static int notify_is_for(const HlPgFrame *f, const char *channel, char *got,
                         size_t gotsz)
{
    if (got && gotsz) got[0] = '\0';
    HlPgCursor c;
    hl_pg_cursor_init(&c, f);
    (void)hl_pg_get_i32(&c);                        /* sender pid */
    const char *ch = hl_pg_get_cstr(&c);
    if (!ch || hl_pg_cursor_err(&c)) return 0;
    if (got && gotsz) snprintf(got, gotsz, "%s", ch);
    return !channel || strcasecmp(ch, channel) == 0;
}

static int wait_notify(HlPgConn *conn, const char *channel, int timeout_ms,
                       HlPgNotifyOtherFn on_other, void *ud)
{
    int fd = hl_db_transport_fd(conn->transport);
    if (fd < 0) return -1;
    if (timeout_ms < 0) timeout_ms = 0;
    int64_t deadline = mono_ms() + timeout_ms;

    for (;;) {
        /* 1. Consume any COMPLETE frame already buffered. poll(2) watches the
         * socket, not our accumulation buffer, so a frame received alongside an
         * earlier one would be invisible to poll and must be drained here first. */
        pg_rbuf_compact(conn);
        HlPgFrame f;
        size_t consumed = 0;
        HlPgResult r = hl_pg_frame_next(conn->rbuf, conn->rlen, &f, &consumed);
        if (r == HL_PG_ERR) {
            set_err(conn->errmsg, sizeof conn->errmsg, "malformed message from server");
            return -1;
        }
        if (r == HL_PG_OK) {
            uint8_t type = f.type;
            conn->consumed = consumed;   /* compacted at the top of the next loop */
            if (type == HL_PG_B_NOTIFY) {
                char got[64];
                if (notify_is_for(&f, channel, got, sizeof got)) return 1;
                /* Another channel LISTENed on this connection: it did not
                 * wake this wait (audit 7 L4). Report it and keep waiting. */
                if (on_other && got[0]) on_other(ud, got);
            }
            continue;   /* a stray Notice / ParameterStatus: skip, check for more */
        }

        /* 2. NEED_MORE: poll the socket with the REMAINING time (so stray async
         * frames can't extend the total wait past timeout_ms). */
        int remaining = (int)(deadline - mono_ms());
        if (remaining <= 0) return 0;   /* timed out, no notification */
        /* Under TLS the next message may already be decrypted and held by
         * the session: poll would not see it, and the wait ran out with a
         * notification sitting in the buffer. Read it instead. */
        if (hl_db_transport_pending(conn->transport) == 0) {
            /* In slices that ask the op's budget (audit 27): a cancelled op or
             * a pool shutting down ends the wait as a timeout instead of
             * holding the pool thread - and the SIGTERM join - for up to a
             * minute. Only whole frames have been consumed, so the connection
             * stays usable. */
            if (pg_wait_stop(NULL)) return 0;
            int slice = remaining < HL_DB_WAIT_SLICE_MS ? remaining
                                                        : HL_DB_WAIT_SLICE_MS;
            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
            int pr = poll(&pfd, 1, slice);
            if (pr < 0) { if (errno == EINTR) continue; return -1; }
            if (pr == 0) continue;          /* the loop re-checks the deadline */
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;   /* dead conn */
        }

        /* 3. Readable: recv one chunk into the buffer (grow if full, bounded like
         * conn_next_frame), then loop back to parse. */
        if (conn->rlen == conn->rcap) {
            size_t ncap = conn->rcap ? conn->rcap * 2 : HL_PG_RBUF_INITIAL;
            if (ncap > (size_t)HL_PG_MAX_MSG + 8192) {
                set_err(conn->errmsg, sizeof conn->errmsg, "server message exceeds limit");
                return -1;
            }
            uint8_t *nb = realloc(conn->rbuf, ncap);
            if (!nb) { set_err(conn->errmsg, sizeof conn->errmsg, "out of memory"); return -1; }
            conn->rbuf = nb;
            conn->rcap = ncap;
        }
        /* Sliced, bounded by what is left of the wait: over TLS a record's
         * first byte made the socket readable, and a blocking read then waited
         * for the rest of it under SO_RCVTIMEO - past the wait's own timeout,
         * for ever with ?read_timeout=0 (audit 23). */
        remaining = (int)(deadline - mono_ms());
        if (remaining <= 0) remaining = 1;
        ssize_t n = hl_db_transport_recv_sliced(conn->transport,
                                                conn->rbuf + conn->rlen,
                                                conn->rcap - conn->rlen,
                                                remaining, NULL, NULL);
        if (n <= 0) { set_err(conn->errmsg, sizeof conn->errmsg,
                              n < 0 ? "read failed or timed out"
                                    : "connection closed by server");
                      return -1; }
        conn->rlen += (size_t)n;
    }
}

int hl_pg_wait_notify(HlPgConn *conn, const char *channel, int timeout_ms,
                      HlPgNotifyOtherFn on_other, void *ud)
{
    if (!conn || !conn->transport) return -1;
    int rc = wait_notify(conn, channel, timeout_ms, on_other, ud);
    /* Every failure leaves the reply stream in an unknown state (a frame half
     * read, a dead socket): the connection refuses further use like after a
     * failed query read, rather than hand the next query this one's bytes
     * (audit 9 L5). */
    if (rc < 0) conn->broken = 1;
    return rc;
}

#endif /* HL_PG_NO_TLS: end of the query / notify connection-layer region */
