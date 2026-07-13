/*
 * cap/pg_conn.c: PostgreSQL DSN parsing, connect, and startup handshake
 *
 * Phase 2 of the PostgreSQL backend: a blocking, plaintext connection with
 * trust / cleartext-password auth. TLS (via KlTls, mirroring cap/smtp.c) and
 * md5 / SCRAM-SHA-256 auth are Phase 3. The receive path feeds the untrusted
 * pgwire reader; the DSN parser bounds every copy and rejects oversized
 * components rather than truncating.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/pg_conn.h"
#include "hull/cap/pgwire.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define PG_SEND_FLAGS MSG_NOSIGNAL
#else
#define PG_SEND_FLAGS 0
#endif

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

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode src[0,srclen) into dst (size dstsize, always terminated).
 * Returns 0 on success, -1 if it does not fit or an escape is malformed. */
static int dsn_decode(char *dst, size_t dstsize, const char *src, size_t srclen)
{
    size_t o = 0;
    for (size_t i = 0; i < srclen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (i + 2 >= srclen) return -1;
            int hi = hexval(src[i + 1]), lo = hexval(src[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (o + 1 >= dstsize) return -1;   /* keep room for the NUL */
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return 0;
}

static int starts_with(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

int hl_pg_dsn_parse(const char *dsn, HlPgDsn *out, char *errbuf, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->port, sizeof out->port, "%s", "5432");

    if (!dsn) { set_err(errbuf, errlen, "null DSN"); return -1; }

    const char *p = dsn;
    if (starts_with(p, "postgres://"))         p += strlen("postgres://");
    else if (starts_with(p, "postgresql://"))  p += strlen("postgresql://");
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
            if (dsn_decode(out->user, sizeof out->user, p, ulen) != 0 ||
                dsn_decode(out->password, sizeof out->password, pw, plen) != 0) {
                set_err(errbuf, errlen, "DSN user/password too long or malformed");
                return -1;
            }
        } else if (dsn_decode(out->user, sizeof out->user, p, ui_len) != 0) {
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
    if (host_len == 0 || dsn_decode(out->host, sizeof out->host,
                                    hostport, host_len) != 0) {
        set_err(errbuf, errlen, "DSN host missing or too long"); return -1;
    }

    /* Path (dbname) and query string follow the authority. */
    const char *tail = p + auth_len;              /* points at '/' or '?' or '\0' */
    const char *qmark = strchr(tail, '?');
    if (*tail == '/') {
        const char *db = tail + 1;
        size_t db_len = (qmark ? (size_t)(qmark - db) : strlen(db));
        if (dsn_decode(out->dbname, sizeof out->dbname, db, db_len) != 0) {
            set_err(errbuf, errlen, "DSN database name too long"); return -1;
        }
    }
    if (qmark) {
        /* Only sslmode is consumed today; other params are ignored. */
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
                    if (dsn_decode(out->sslmode, sizeof out->sslmode, v, vlen) != 0) {
                        set_err(errbuf, errlen, "DSN sslmode too long"); return -1;
                    }
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

/* ── Blocking transport ───────────────────────────────────────────── */

/* TCP connect with a bounded wait (non-blocking connect + select), mirroring
 * cap/smtp.c. Returns a blocking fd, or -1. */
static int pg_connect(const char *host, const char *port, int timeout_ms)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rc = select(fd + 1, NULL, &wf, NULL, timeout_ms > 0 ? &tv : NULL);
        if (rc > 0) {
            int soerr = 0;
            socklen_t l = sizeof soerr;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &l);
            rc = soerr == 0 ? 0 : -1;
        } else {
            rc = -1;
        }
    }
    freeaddrinfo(res);
    if (rc < 0) { close(fd); return -1; }

    fcntl(fd, F_SETFL, flags);   /* restore blocking */
    return fd;
}

static int conn_send(HlPgConn *conn, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n;
        do { n = send(conn->fd, buf + sent, len - sent, PG_SEND_FLAGS); }
        while (n < 0 && errno == EINTR);
        if (n <= 0) { set_err(conn->errmsg, sizeof conn->errmsg, "socket write failed");
                      return -1; }
        sent += (size_t)n;
    }
    return 0;
}

/* Read one frame. The returned body is valid until the next call, which
 * compacts the previously-consumed bytes. Returns 0 / -1. */
static int conn_next_frame(HlPgConn *conn, HlPgFrame *f)
{
    if (conn->consumed > 0) {
        memmove(conn->rbuf, conn->rbuf + conn->consumed,
                conn->rlen - conn->consumed);
        conn->rlen -= conn->consumed;
        conn->consumed = 0;
    }
    for (;;) {
        size_t consumed = 0;
        HlPgResult r = hl_pg_frame_next(conn->rbuf, conn->rlen, f, &consumed);
        if (r == HL_PG_OK) { conn->consumed = consumed; return 0; }
        if (r == HL_PG_ERR) {
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "malformed message from server");
            return -1;
        }
        /* NEED_MORE: grow if the buffer is full (bounded), then read. */
        if (conn->rlen == conn->rcap) {
            size_t ncap = conn->rcap ? conn->rcap * 2 : 8192;
            /* frame_next rejects any length > HL_PG_MAX_MSG, so a legitimate
             * frame never needs more than that plus its 5-byte header. */
            if (ncap > (size_t)HL_PG_MAX_MSG + 8192) {
                set_err(conn->errmsg, sizeof conn->errmsg,
                        "server message exceeds limit");
                return -1;
            }
            uint8_t *nb = realloc(conn->rbuf, ncap);
            if (!nb) { set_err(conn->errmsg, sizeof conn->errmsg, "out of memory");
                       return -1; }
            conn->rbuf = nb;
            conn->rcap = ncap;
        }
        ssize_t n;
        do { n = recv(conn->fd, conn->rbuf + conn->rlen,
                      conn->rcap - conn->rlen, 0); }
        while (n < 0 && errno == EINTR);
        if (n <= 0) {
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "connection closed by server");
            return -1;
        }
        conn->rlen += (size_t)n;
    }
}

/* ── Handshake ────────────────────────────────────────────────────── */

static void conn_teardown(HlPgConn *conn)
{
    if (conn->fd >= 0) { close(conn->fd); conn->fd = -1; }
    free(conn->rbuf);
    conn->rbuf = NULL;
    conn->rcap = conn->rlen = conn->consumed = 0;
}

int hl_pg_conn_start(HlPgConn *conn, int fd, const HlPgDsn *dsn)
{
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;

#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif

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
    int guard = 0;
    for (;;) {
        if (++guard > 1000) {
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "too many messages during handshake");
            conn_teardown(conn);
            return -1;
        }

        HlPgFrame f;
        if (conn_next_frame(conn, &f) != 0) { conn_teardown(conn); return -1; }

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
                authenticated = 1;
            } else if (sub == HL_PG_AUTH_CLEARTEXT) {
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
            } else {
                snprintf(conn->errmsg, sizeof conn->errmsg,
                         "auth method %d needs Phase 3 (TLS + md5/SCRAM)", sub);
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

int hl_pg_conn_open(HlPgConn *conn, const HlPgDsn *dsn, int timeout_ms)
{
    int fd = pg_connect(dsn->host, dsn->port, timeout_ms);
    if (fd < 0) {
        memset(conn, 0, sizeof(*conn));
        conn->fd = -1;
        snprintf(conn->errmsg, sizeof conn->errmsg,
                 "cannot connect to %s:%s", dsn->host, dsn->port);
        return -1;
    }
    return hl_pg_conn_start(conn, fd, dsn);
}

void hl_pg_conn_close(HlPgConn *conn)
{
    if (!conn) return;
    if (conn->fd >= 0) {
        HlPgWriter w;
        hl_pg_writer_init(&w);
        hl_pg_build_terminate(&w);
        if (!w.err) (void)conn_send(conn, w.buf, w.len);   /* best effort */
        hl_pg_writer_free(&w);
    }
    conn_teardown(conn);
}
