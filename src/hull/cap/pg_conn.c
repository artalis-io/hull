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
/* SCRAM pulls in cap/crypto (mbedTLS). The DSN / rewriter fuzz harnesses,
 * which link this file for its pure functions, define HL_PG_NO_SCRAM to
 * compile crypto-free. base64 below stays available either way. */
#ifndef HL_PG_NO_SCRAM
#include "hull/cap/crypto.h"
#endif
/* TLS transport (Phase 3b.2). The pure-parser fuzzers define HL_PG_NO_TLS to
 * stay free of Keel; the real build and tests keep raw send/recv when no TLS
 * session is attached. */
#ifndef HL_PG_NO_TLS
#include "hull/shared/tls_client.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
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

/* Transport helpers. Once the TLS handshake has attached a session
 * (conn->tls != NULL) all bytes tunnel through it; otherwise raw socket
 * send/recv. HL_PG_NO_TLS (fuzzers) drops the TLS branch entirely. */
static ssize_t io_send(HlPgConn *conn, const uint8_t *buf, size_t len)
{
#ifndef HL_PG_NO_TLS
    if (conn->tls)
        return hl_tls_client_write(conn->fd, conn->tls, buf, len);
#endif
    ssize_t n;
    do { n = send(conn->fd, buf, len, PG_SEND_FLAGS); }
    while (n < 0 && errno == EINTR);
    return n;
}

static ssize_t io_recv(HlPgConn *conn, uint8_t *buf, size_t len)
{
#ifndef HL_PG_NO_TLS
    if (conn->tls)
        return hl_tls_client_read(conn->fd, conn->tls, buf, len);
#endif
    ssize_t n;
    do { n = recv(conn->fd, buf, len, 0); }
    while (n < 0 && errno == EINTR);
    return n;
}

static int conn_send(HlPgConn *conn, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = io_send(conn, buf + sent, len - sent);
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
        ssize_t n = io_recv(conn, conn->rbuf + conn->rlen,
                            conn->rcap - conn->rlen);
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
#ifndef HL_PG_NO_TLS
    if (conn->tls) { hl_tls_client_free(conn->tls); conn->tls = NULL; }
#endif
    if (conn->fd >= 0) { close(conn->fd); conn->fd = -1; }
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
} PgScram;

/* Handle one SASL AuthenticationRequest sub-message. Returns 0 to continue
 * the handshake, -1 on failure (conn->errmsg set). */
static int scram_handle(HlPgConn *conn, PgScram *sc, const HlPgDsn *dsn,
                        int32_t sub, HlPgCursor *c)
{
    if (sub == HL_PG_AUTH_SASL) {
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
            hl_pg_b64_encode(rnd, sizeof rnd,
                             sc->client_nonce, sizeof sc->client_nonce) < 0) {
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
                    char ib[16];
                    if (vlen < sizeof ib) { memcpy(ib, val, vlen); ib[vlen] = '\0'; iters = atoi(ib); }
                }
            }
            if (!comma) break;
            p = comma + 1;
        }
        if (!rnonce[0] || !salt_b64[0] || iters <= 0) {
            set_err(conn->errmsg, sizeof conn->errmsg, "invalid SCRAM server-first");
            return -1;
        }
        if (strncmp(rnonce, sc->client_nonce, strlen(sc->client_nonce)) != 0) {
            set_err(conn->errmsg, sizeof conn->errmsg, "SCRAM nonce mismatch");
            return -1;
        }
        uint8_t salt[128];
        size_t salt_len = 0;
        if (hl_pg_b64_decode(salt_b64, strlen(salt_b64), salt, sizeof salt, &salt_len) != 0) {
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
        if (hl_pg_b64_encode(proof, 32, proof_b64, sizeof proof_b64) < 0) return -1;
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
        if (hl_pg_b64_decode((const char *)sfp + 2, rem - 2, got, sizeof got, &gl) != 0
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
        return 0;   /* server sends AuthenticationOk next */
    }

    set_err(conn->errmsg, sizeof conn->errmsg, "unsupported SASL sub-message");
    return -1;
}
#endif /* HL_PG_NO_SCRAM */

/* Run the startup + auth handshake on an already-connected fd. When @p tls is
 * non-NULL the socket has already completed a TLS handshake and every byte
 * tunnels through it; ownership of @p tls transfers to conn (freed by
 * conn_teardown). */
static int pg_start(HlPgConn *conn, int fd, const HlPgDsn *dsn,
                    struct HlTlsClient *tls)
{
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;
#ifndef HL_PG_NO_TLS
    conn->tls = tls;
#else
    (void)tls;
#endif

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
            } else if (sub == HL_PG_AUTH_SASL ||
                       sub == HL_PG_AUTH_SASL_CONTINUE ||
                       sub == HL_PG_AUTH_SASL_FINAL) {
#ifndef HL_PG_NO_SCRAM
                if (scram_handle(conn, &scram, dsn, sub, &c) != 0) {
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
    return pg_start(conn, fd, dsn, NULL);
}

/* ── TLS negotiation (SSLRequest / sslmode) ───────────────────────── */

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

HlPgSslDecision hl_pg_ssl_negotiate(int fd, HlPgSslMode mode,
                                    char *errbuf, size_t errlen)
{
    if (mode == HL_PG_SSLMODE_DISABLE) return HL_PG_SSL_PLAINTEXT;

    /* SSLRequest: length(8) then request code 80877103 (0x04d2162f). */
    static const uint8_t req[8] = { 0, 0, 0, 8, 0x04, 0xd2, 0x16, 0x2f };
    size_t sent = 0;
    while (sent < sizeof req) {
        ssize_t n;
        do { n = send(fd, req + sent, sizeof req - sent, PG_SEND_FLAGS); }
        while (n < 0 && errno == EINTR);
        if (n <= 0) { set_err(errbuf, errlen, "failed to send SSLRequest");
                      return HL_PG_SSL_FAIL; }
        sent += (size_t)n;
    }

    uint8_t resp;
    ssize_t n;
    do { n = recv(fd, &resp, 1, 0); } while (n < 0 && errno == EINTR);
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
    int fd = pg_connect(dsn->host, dsn->port, timeout_ms);
    if (fd < 0) {
        memset(conn, 0, sizeof(*conn));
        conn->fd = -1;
        snprintf(conn->errmsg, sizeof conn->errmsg,
                 "cannot connect to %s:%s", dsn->host, dsn->port);
        return -1;
    }

    int mode = hl_pg_sslmode_parse(dsn->sslmode);
    if (mode < 0) {
        memset(conn, 0, sizeof(*conn));
        conn->fd = -1;
        snprintf(conn->errmsg, sizeof conn->errmsg,
                 "unknown sslmode: %s", dsn->sslmode);
        close(fd);
        return -1;
    }
    if (mode != HL_PG_SSLMODE_DISABLE) {
        char e[128] = {0};
        HlPgSslDecision d = hl_pg_ssl_negotiate(fd, (HlPgSslMode)mode, e, sizeof e);
        if (d == HL_PG_SSL_FAIL) {
            memset(conn, 0, sizeof(*conn));
            conn->fd = -1;
            snprintf(conn->errmsg, sizeof conn->errmsg, "%s", e);
            close(fd);
            return -1;
        }
        if (d == HL_PG_SSL_USE_TLS) {
#ifndef HL_PG_NO_TLS
            /* verify-ca / verify-full check the chain + hostname against the
             * embedded CA bundle; require / prefer take the session as-is. */
            int verify = (mode == HL_PG_SSLMODE_VERIFY);
            struct HlTlsClient *tls =
                hl_tls_client_handshake(fd, dsn->host, verify, timeout_ms);
            if (!tls) {
                memset(conn, 0, sizeof(*conn));
                conn->fd = -1;
                snprintf(conn->errmsg, sizeof conn->errmsg,
                         "TLS handshake with %s failed", dsn->host);
                close(fd);
                return -1;
            }
            return pg_start(conn, fd, dsn, tls);
#else
            memset(conn, 0, sizeof(*conn));
            conn->fd = -1;
            set_err(conn->errmsg, sizeof conn->errmsg,
                    "TLS not available in this build");
            close(fd);
            return -1;
#endif
        }
        /* PLAINTEXT: server declined TLS and sslmode permits fallback. */
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

/* ── Standard base64 + SCRAM-SHA-256 ──────────────────────────────── */

static const char B64E[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int hl_pg_b64_encode(const uint8_t *in, size_t inlen, char *out, size_t outsize)
{
    size_t need = ((inlen + 2) / 3) * 4;
    if (need + 1 > outsize) return -1;
    size_t o = 0;
    for (size_t i = 0; i < inlen; i += 3) {
        int rem = (int)(inlen - i);
        uint32_t n = (uint32_t)in[i] << 16;
        if (rem > 1) n |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) n |= (uint32_t)in[i + 2];
        out[o++] = B64E[(n >> 18) & 63];
        out[o++] = B64E[(n >> 12) & 63];
        out[o++] = rem > 1 ? B64E[(n >> 6) & 63] : '=';
        out[o++] = rem > 2 ? B64E[n & 63] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

int hl_pg_b64_decode(const char *in, size_t inlen,
                     uint8_t *out, size_t outsize, size_t *outlen)
{
    int rev[256];
    for (int i = 0; i < 256; i++) rev[i] = -1;
    for (int i = 0; i < 64; i++) rev[(unsigned char)B64E[i]] = i;

    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') break;
        int v = rev[c];
        if (v < 0) return -1;   /* strict: reject any non-alphabet byte */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= outsize) return -1;
            out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    if (outlen) *outlen = o;
    return 0;
}

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

    uint8_t u[32], t[32];
    int rc = hl_cap_crypto_hmac_sha256((const uint8_t *)pw, pw_len,
                                       msg, salt_len + 4, u);
    free(msg);
    if (rc != 0) return -1;
    memcpy(t, u, 32);

    for (int i = 1; i < iters; i++) {
        if (hl_cap_crypto_hmac_sha256((const uint8_t *)pw, pw_len, u, 32, u) != 0)
            return -1;
        for (int j = 0; j < 32; j++) t[j] = (uint8_t)(t[j] ^ u[j]);
    }
    memcpy(out, t, 32);
    return 0;
}

int hl_pg_scram_proof(const char *password, size_t pw_len,
                      const uint8_t *salt, size_t salt_len, int iterations,
                      const char *auth_msg, size_t auth_msg_len,
                      uint8_t client_proof[32], uint8_t server_sig[32])
{
    uint8_t salted[32], client_key[32], stored_key[32];
    uint8_t client_sig[32], server_key[32];

    if (scram_pbkdf2(password, pw_len, salt, salt_len, iterations, salted) != 0)
        return -1;
    if (hl_cap_crypto_hmac_sha256(salted, 32,
            (const uint8_t *)"Client Key", 10, client_key) != 0) return -1;
    if (hl_cap_crypto_sha256(client_key, 32, stored_key) != 0) return -1;
    if (hl_cap_crypto_hmac_sha256(stored_key, 32,
            (const uint8_t *)auth_msg, auth_msg_len, client_sig) != 0) return -1;
    for (int i = 0; i < 32; i++)
        client_proof[i] = (uint8_t)(client_key[i] ^ client_sig[i]);

    if (hl_cap_crypto_hmac_sha256(salted, 32,
            (const uint8_t *)"Server Key", 10, server_key) != 0) return -1;
    if (hl_cap_crypto_hmac_sha256(server_key, 32,
            (const uint8_t *)auth_msg, auth_msg_len, server_sig) != 0) return -1;

    /* Scrub the derived secrets. */
    pg_secure_zero(salted, sizeof salted);
    pg_secure_zero(client_key, sizeof client_key);
    pg_secure_zero(server_key, sizeof server_key);
    return 0;
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

int hl_pg_query(HlPgConn *conn, const char *sql,
                const HlPgParam *params, int nparams,
                HlPgDescCb desc_cb, HlPgRowCb row_cb, void *cb_ctx,
                int64_t *affected)
{
    if (affected) *affected = -1;
    if (!conn || conn->fd < 0) {
        if (conn) set_err(conn->errmsg, sizeof conn->errmsg, "not connected");
        return -1;
    }
    conn->errmsg[0] = '\0';
    if (nparams < 0 || nparams > 65535) {
        set_err(conn->errmsg, sizeof conn->errmsg, "invalid parameter count");
        return -1;
    }

    /* Rewrite '?' -> '$n' into a buffer sized for the worst case. */
    size_t rwcap = strlen(sql) + (size_t)nparams * 12 + 32;
    char *rw = malloc(rwcap);
    if (!rw) { set_err(conn->errmsg, sizeof conn->errmsg, "out of memory"); return -1; }
    int found = 0;
    if (hl_pg_rewrite_sql(sql, rw, rwcap, &found) != 0) {
        free(rw);
        set_err(conn->errmsg, sizeof conn->errmsg, "failed to rewrite SQL placeholders");
        return -1;
    }
    if (found != nparams) {
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
    hl_pg_put_i16(&w, (int16_t)nparams);
    for (int i = 0; i < nparams; i++) {
        if (!params[i].text) {
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
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_pg_writer_free(&w);
    if (se) {
        if (!conn->errmsg[0])
            set_err(conn->errmsg, sizeof conn->errmsg, "failed to send query");
        return -1;
    }

    /* Read the response stream until ReadyForQuery. */
    int had_error = 0;
    int stop_rows = 0;
    for (;;) {
        HlPgFrame f;
        if (conn_next_frame(conn, &f) != 0) return -1;

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
        case HL_PG_B_READY:
            conn->tx_status = hl_pg_get_u8(&c);
            return had_error ? -1 : 0;
        default:
            break;
        }
    }
}
