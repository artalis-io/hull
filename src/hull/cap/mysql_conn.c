/*
 * cap/mysql_conn.c: MySQL / MariaDB connection
 *
 * Phase 1b: the pure DSN parser only (percent-decode + bounded field split),
 * kept free of any socket / TLS / crypto dependency so the fuzzer links it
 * standalone. The handshake, auth plugins, TLS, and query protocols land in
 * later phases (added here behind transport guards, mirroring cap/pg_conn.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/mysql_conn.h"

#include <stdio.h>
#include <string.h>

/* ── DSN parsing (pure; mirrors hl_pg_dsn_parse) ──────────────────── */

static void set_err(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf && errlen) snprintf(errbuf, errlen, "%s", msg);
}

static int starts_with(const char *s, const char *pfx)
{
    size_t n = strlen(pfx);
    return strncmp(s, pfx, n) == 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode src[0,slen) into dst[0,dstsz) as a NUL-terminated string.
 * Returns 0, or -1 if it would overflow dst or a %-escape is truncated/bad. */
static int dsn_decode(char *dst, size_t dstsz, const char *src, size_t slen)
{
    size_t o = 0;
    for (size_t i = 0; i < slen; i++) {
        char c = src[i];
        if (c == '%') {
            if (i + 2 >= slen) return -1;
            int hi = hexval((unsigned char)src[i + 1]);
            int lo = hexval((unsigned char)src[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        if (o + 1 >= dstsz) return -1;   /* leave room for the NUL */
        dst[o++] = c;
    }
    dst[o] = '\0';
    return 0;
}

int hl_my_dsn_parse(const char *dsn, HlMyDsn *out, char *errbuf, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->port, sizeof out->port, "%s", "3306");

    if (!dsn) { set_err(errbuf, errlen, "null DSN"); return -1; }

    const char *p = dsn;
    if (starts_with(p, "mysql://"))        p += strlen("mysql://");
    else if (starts_with(p, "mariadb://")) p += strlen("mariadb://");
    else { set_err(errbuf, errlen, "DSN must start with mysql:// or mariadb://");
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

    /* host[:port]. (IPv6 literals need [..]; not parsed yet, like the PG side.) */
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
    const char *tail = p + auth_len;              /* '/' or '?' or '\0' */
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

void hl_my_dsn_scrub(HlMyDsn *dsn)
{
    if (!dsn) return;
    volatile char *p = (volatile char *)dsn->password;
    for (size_t i = 0; i < sizeof dsn->password; i++) p[i] = 0;
}

/* ── Auth + connection (crypto + socket) ──────────────────────────── */
#ifndef HL_MY_NO_AUTH
#include "hull/cap/crypto.h"
#include "hull/cap/mysqlwire.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>

int hl_my_native_password_scramble(const char *password,
                                   const uint8_t scramble[HL_MY_SCRAMBLE_LEN],
                                   uint8_t out[HL_MY_SCRAMBLE_LEN])
{
    if (!password || !password[0])
        return 0;   /* empty password -> zero-length auth response */

    uint8_t h1[HL_MY_SCRAMBLE_LEN], h2[HL_MY_SCRAMBLE_LEN];
    uint8_t cat[HL_MY_SCRAMBLE_LEN * 2], h3[HL_MY_SCRAMBLE_LEN];
    if (hl_cap_crypto_sha1(password, strlen(password), h1) != 0) return -1;
    if (hl_cap_crypto_sha1(h1, HL_MY_SCRAMBLE_LEN, h2) != 0) return -1;
    memcpy(cat, scramble, HL_MY_SCRAMBLE_LEN);
    memcpy(cat + HL_MY_SCRAMBLE_LEN, h2, HL_MY_SCRAMBLE_LEN);
    if (hl_cap_crypto_sha1(cat, sizeof cat, h3) != 0) return -1;
    for (int i = 0; i < HL_MY_SCRAMBLE_LEN; i++)
        out[i] = (uint8_t)(h1[i] ^ h3[i]);
    return HL_MY_SCRAMBLE_LEN;
}

/* ── Blocking transport ───────────────────────────────────────────── */

static void conn_set_err(HlMyConn *conn, const char *msg)
{
    if (conn) snprintf(conn->errmsg, sizeof conn->errmsg, "%s", msg);
}

/* TCP connect with a bounded wait (non-blocking connect + select), mirroring
 * cap/pg_conn.c. Returns a blocking fd, or -1. */
static int my_connect(const char *host, const char *port, int timeout_ms)
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
        fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        rc = select(fd + 1, NULL, &wf, NULL, timeout_ms > 0 ? &tv : NULL);
        if (rc > 0) {
            int soerr = 0; socklen_t l = sizeof soerr;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &l);
            rc = soerr == 0 ? 0 : -1;
        } else rc = -1;
    }
    freeaddrinfo(res);
    if (rc < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, flags);   /* restore blocking */
    return fd;
}

static int conn_send(HlMyConn *conn, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n;
        do { n = send(conn->fd, buf + sent, len - sent, 0); }
        while (n < 0 && errno == EINTR);
        if (n <= 0) { conn_set_err(conn, "socket write failed"); return -1; }
        sent += (size_t)n;
    }
    return 0;
}

/* Read one packet. The returned body is valid until the next call, which
 * compacts the previously-consumed bytes. Returns 0 / -1. */
static int conn_next_frame(HlMyConn *conn, HlMyFrame *f)
{
    if (conn->consumed > 0) {
        memmove(conn->rbuf, conn->rbuf + conn->consumed,
                conn->rlen - conn->consumed);
        conn->rlen -= conn->consumed;
        conn->consumed = 0;
    }
    for (;;) {
        size_t consumed = 0;
        HlMyResult r = hl_my_frame_next(conn->rbuf, conn->rlen, f, &consumed);
        if (r == HL_MY_OK) { conn->consumed = consumed; return 0; }
        if (r == HL_MY_ERR) { conn_set_err(conn, "malformed message from server");
                              return -1; }
        /* NEED_MORE: grow if full (bounded by the 24-bit packet cap), then read. */
        if (conn->rlen == conn->rcap) {
            size_t ncap = conn->rcap ? conn->rcap * 2 : HL_MY_RECV_BUF_INIT;
            if (ncap > (size_t)HL_MY_MAX_MSG + HL_MY_RECV_BUF_INIT) {
                conn_set_err(conn, "server message exceeds limit"); return -1;
            }
            uint8_t *nb = realloc(conn->rbuf, ncap);
            if (!nb) { conn_set_err(conn, "out of memory"); return -1; }
            conn->rbuf = nb; conn->rcap = ncap;
        }
        ssize_t n;
        do { n = recv(conn->fd, conn->rbuf + conn->rlen, conn->rcap - conn->rlen, 0); }
        while (n < 0 && errno == EINTR);
        if (n <= 0) { conn_set_err(conn, "connection closed by server"); return -1; }
        conn->rlen += (size_t)n;
    }
}

void hl_my_conn_close(HlMyConn *conn)
{
    if (!conn) return;
    if (conn->fd >= 0) { close(conn->fd); conn->fd = -1; }
    free(conn->rbuf);
    conn->rbuf = NULL;
    conn->rlen = conn->rcap = conn->consumed = 0;
}

/* ── Handshake ────────────────────────────────────────────────────── */

/* Send a bare auth-data packet (the AuthSwitch / AuthMoreData continuation). */
static int send_auth_data(HlMyConn *conn, uint8_t seq,
                          const uint8_t *data, int len)
{
    HlMyWriter w; hl_my_writer_init(&w);
    size_t m = hl_my_packet_begin(&w, seq);
    if (data && len > 0) hl_my_put_bytes(&w, data, (size_t)len);
    hl_my_packet_end(&w, m);
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_my_writer_free(&w);
    return se ? -1 : 0;
}

int hl_my_conn_start(HlMyConn *conn, int fd, const HlMyDsn *dsn)
{
    memset(conn, 0, sizeof *conn);
    conn->fd = fd;

    HlMyFrame f;
    if (conn_next_frame(conn, &f) != 0) { hl_my_conn_close(conn); return -1; }

    /* A pre-handshake ERR (e.g. host not allowed) starts with 0xFF. */
    if (f.body_len > 0 && f.body[0] == HL_MY_PKT_ERR) {
        HlMyErr e;
        if (hl_my_parse_err(&f, 0, &e) == 0 && e.message_len)
            snprintf(conn->errmsg, sizeof conn->errmsg, "server: %.*s",
                     (int)e.message_len, e.message);
        else conn_set_err(conn, "server rejected connection");
        hl_my_conn_close(conn); return -1;
    }

    HlMyHandshake hs;
    if (hl_my_parse_handshake(&f, &hs) != 0) {
        conn_set_err(conn, "malformed server handshake"); hl_my_conn_close(conn);
        return -1;
    }

    uint32_t caps = HL_MY_CLIENT_PROTOCOL_41 | HL_MY_CLIENT_SECURE_CONNECTION
                  | HL_MY_CLIENT_PLUGIN_AUTH | HL_MY_CLIENT_TRANSACTIONS
                  | HL_MY_CLIENT_MULTI_STATEMENTS | HL_MY_CLIENT_MULTI_RESULTS;
    if (dsn->dbname[0]) caps |= HL_MY_CLIENT_CONNECT_WITH_DB;
    conn->capabilities = caps;

    uint8_t auth[HL_MY_SCRAMBLE_LEN];
    int authlen = hl_my_native_password_scramble(dsn->password, hs.scramble, auth);
    if (authlen < 0) { conn_set_err(conn, "auth scramble failed");
                       hl_my_conn_close(conn); return -1; }

    HlMyWriter w; hl_my_writer_init(&w);
    hl_my_build_handshake_response(
        &w, (uint8_t)(f.seq + 1), caps,
        hs.charset ? hs.charset : HL_MY_DEFAULT_CHARSET, dsn->user,
        auth, (size_t)authlen, dsn->dbname[0] ? dsn->dbname : NULL,
        "mysql_native_password");
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_my_writer_free(&w);
    if (se) { conn_set_err(conn, "failed to send handshake response");
              hl_my_conn_close(conn); return -1; }

    /* Read the auth result: OK, ERR, or an AuthSwitchRequest (0xFE). */
    for (;;) {
        if (conn_next_frame(conn, &f) != 0) { hl_my_conn_close(conn); return -1; }
        if (f.body_len == 0) continue;
        uint8_t hdr = f.body[0];

        if (hdr == HL_MY_PKT_OK) return 0;   /* authenticated */

        if (hdr == HL_MY_PKT_ERR) {
            HlMyErr e;
            if (hl_my_parse_err(&f, 1, &e) == 0 && e.message_len)
                snprintf(conn->errmsg, sizeof conn->errmsg, "auth failed: %.*s",
                         (int)e.message_len, e.message);
            else conn_set_err(conn, "authentication failed");
            hl_my_conn_close(conn); return -1;
        }

        if (hdr == HL_MY_PKT_EOF) {          /* 0xFE AuthSwitchRequest */
            HlMyCursor c; hl_my_cursor_init(&c, &f);
            (void)hl_my_get_u8(&c);          /* 0xFE */
            const char *plugin = hl_my_get_cstr(&c);
            size_t dlen = 0;
            const uint8_t *data = hl_my_get_eof_str(&c, &dlen);
            if (plugin && strcmp(plugin, "mysql_native_password") == 0) {
                uint8_t sc[HL_MY_SCRAMBLE_LEN];
                memset(sc, 0, sizeof sc);
                memcpy(sc, data, dlen >= HL_MY_SCRAMBLE_LEN
                                 ? (size_t)HL_MY_SCRAMBLE_LEN : dlen);
                uint8_t a2[HL_MY_SCRAMBLE_LEN];
                int n2 = hl_my_native_password_scramble(dsn->password, sc, a2);
                if (n2 < 0 || send_auth_data(conn, (uint8_t)(f.seq + 1), a2, n2) != 0) {
                    conn_set_err(conn, "auth-switch response failed");
                    hl_my_conn_close(conn); return -1;
                }
                continue;   /* read the next result (OK / ERR) */
            }
            snprintf(conn->errmsg, sizeof conn->errmsg,
                     "unsupported auth plugin '%s' (native only until Phase 5)",
                     plugin ? plugin : "?");
            hl_my_conn_close(conn); return -1;
        }

        conn_set_err(conn, "unexpected auth response from server");
        hl_my_conn_close(conn); return -1;
    }
}

int hl_my_conn_open(HlMyConn *conn, const HlMyDsn *dsn, int timeout_ms)
{
    int fd = my_connect(dsn->host, dsn->port, timeout_ms);
    if (fd < 0) {
        memset(conn, 0, sizeof *conn);
        conn->fd = -1;
        snprintf(conn->errmsg, sizeof conn->errmsg,
                 "could not connect to %s:%s", dsn->host, dsn->port);
        return -1;
    }
    return hl_my_conn_start(conn, fd, dsn);
}

/* ── Queries (COM_QUERY text protocol) ────────────────────────────── */

/* An EOF packet body is shorter than an OK/row body that also starts 0xFE. */
#define HL_MY_EOF_MAX_LEN  9

static void free_names(char **names, int n)
{
    if (!names) return;
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

int hl_my_conn_query(HlMyConn *conn, const char *sql,
                     HlMyDescCb desc_cb, HlMyRowCb row_cb, void *cb_ctx,
                     int64_t *affected)
{
    if (affected) *affected = -1;

    /* COM_QUERY is a fresh command: the packet sequence restarts at 0. */
    HlMyWriter w; hl_my_writer_init(&w);
    size_t m = hl_my_packet_begin(&w, 0);
    hl_my_put_u8(&w, HL_MY_COM_QUERY);
    hl_my_put_bytes(&w, sql, strlen(sql));
    hl_my_packet_end(&w, m);
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_my_writer_free(&w);
    if (se) { conn_set_err(conn, "failed to send query"); return -1; }

    HlMyFrame f;
    if (conn_next_frame(conn, &f) != 0) return -1;
    if (f.body_len == 0) { conn_set_err(conn, "empty query response"); return -1; }
    uint8_t hdr = f.body[0];

    if (hdr == HL_MY_PKT_OK) {                 /* no result set (DML / DDL) */
        HlMyOk ok;
        if (hl_my_parse_ok(&f, &ok) == 0) {
            conn->last_insert_id = ok.last_insert_id;
            if (affected) *affected = (int64_t)ok.affected_rows;
        }
        return 0;
    }
    if (hdr == HL_MY_PKT_ERR) {
        HlMyErr e;
        if (hl_my_parse_err(&f, 1, &e) == 0 && e.message_len)
            snprintf(conn->errmsg, sizeof conn->errmsg, "query failed: %.*s",
                     (int)e.message_len, e.message);
        else conn_set_err(conn, "query failed");
        return -1;
    }
    if (hdr == HL_MY_PKT_LOCAL_INFILE) {
        conn_set_err(conn, "LOCAL INFILE responses are not supported");
        return -1;
    }

    /* Result set: first packet is the column count (lenenc int). */
    HlMyCursor cc; hl_my_cursor_init(&cc, &f);
    uint64_t ncols64 = hl_my_get_lenenc_int(&cc, NULL);
    if (hl_my_cursor_err(&cc) || ncols64 == 0 || ncols64 > HL_MY_MAX_COLUMNS) {
        conn_set_err(conn, "malformed column count"); return -1;
    }
    int ncols = (int)ncols64;

    /* Column names live in per-column frames that are invalidated as soon as we
     * read the next frame, so copy them now. */
    char       **names  = calloc((size_t)ncols, sizeof *names);
    HlMyField   *fields = calloc((size_t)ncols, sizeof *fields);
    const char **vals   = calloc((size_t)ncols, sizeof *vals);
    size_t      *lens   = calloc((size_t)ncols, sizeof *lens);
    if (!names || !fields || !vals || !lens) {
        conn_set_err(conn, "out of memory"); goto fail;
    }
    for (int i = 0; i < ncols; i++) {
        if (conn_next_frame(conn, &f) != 0) goto fail;
        HlMyColumn col;
        if (hl_my_parse_column_def(&f, &col) != 0) {
            conn_set_err(conn, "malformed column definition"); goto fail;
        }
        names[i] = malloc(col.name_len + 1);
        if (!names[i]) { conn_set_err(conn, "out of memory"); goto fail; }
        memcpy(names[i], col.name, col.name_len);
        names[i][col.name_len] = '\0';
        fields[i].name = names[i];
        fields[i].type = col.type;
    }

    /* No CLIENT_DEPRECATE_EOF was advertised, so an EOF terminates the column
     * defs (and, below, the rows). */
    if (conn_next_frame(conn, &f) != 0) goto fail;   /* intermediate EOF */
    if (desc_cb) desc_cb(cb_ctx, fields, ncols);

    int stop = 0;
    for (;;) {
        if (conn_next_frame(conn, &f) != 0) goto fail;
        if (f.body_len > 0 && f.body[0] == HL_MY_PKT_EOF && f.body_len < HL_MY_EOF_MAX_LEN)
            break;                              /* end of rows */
        if (f.body_len > 0 && f.body[0] == HL_MY_PKT_ERR) {
            HlMyErr e;
            if (hl_my_parse_err(&f, 1, &e) == 0 && e.message_len)
                snprintf(conn->errmsg, sizeof conn->errmsg, "query failed: %.*s",
                         (int)e.message_len, e.message);
            else conn_set_err(conn, "query failed mid-result");
            goto fail;
        }
        HlMyCursor rc; hl_my_cursor_init(&rc, &f);
        for (int i = 0; i < ncols; i++) {
            if (rc.remaining >= 1 && rc.p[0] == HL_MY_PKT_LOCAL_INFILE) {  /* 0xFB = NULL in a row */
                (void)hl_my_get_u8(&rc);
                vals[i] = NULL; lens[i] = 0;
            } else {
                size_t l = 0;
                vals[i] = (const char *)hl_my_get_lenenc_str(&rc, &l);
                lens[i] = l;
            }
        }
        if (hl_my_cursor_err(&rc)) { conn_set_err(conn, "malformed result row"); goto fail; }
        if (row_cb && !stop && row_cb(cb_ctx, vals, lens, ncols) != 0)
            stop = 1;                           /* keep draining to EOF */
    }

    free_names(names, ncols);
    free(fields); free(vals); free(lens);
    return 0;

fail:
    free_names(names, ncols);
    free(fields); free(vals); free(lens);
    return -1;
}

/* ── Prepared statements (binary protocol) ────────────────────────── */

/* COM_STMT_EXECUTE fixed fields + binary-row null-bitmap layout. */
#define HL_MY_STMT_CURSOR_NONE       0x00  /* CURSOR_TYPE_NO_CURSOR */
#define HL_MY_STMT_ITERATIONS        1u    /* always 1 for a single execute */
#define HL_MY_STMT_NEW_PARAMS_BOUND  1     /* send the param type block this execute */
#define HL_MY_STMT_ROW_NULL_OFFSET   2     /* binary result rows reserve 2 leading bits */

/* Read @p n ColumnDefinition41 frames then the trailing EOF (skipped for n==0).
 * Used to drain the metadata blocks in a COM_STMT_PREPARE_OK response. */
static int drain_defs(HlMyConn *conn, int n)
{
    HlMyFrame f;
    for (int i = 0; i < n; i++)
        if (conn_next_frame(conn, &f) != 0) return -1;
    if (n > 0 && conn_next_frame(conn, &f) != 0) return -1;   /* EOF */
    return 0;
}

/* Decode one binary-protocol value at the cursor into @p out by column type. */
static void decode_binary_value(HlMyCursor *rc, uint8_t type, HlMyVal *out)
{
    switch (type) {
    case HL_MY_TYPE_TINY:
        out->kind = HL_MY_VAL_INT; out->v.i = (int8_t)hl_my_get_u8(rc); return;
    case HL_MY_TYPE_SHORT:
        out->kind = HL_MY_VAL_INT; out->v.i = (int16_t)hl_my_get_u16(rc); return;
    case HL_MY_TYPE_YEAR:
        out->kind = HL_MY_VAL_INT; out->v.i = (uint16_t)hl_my_get_u16(rc); return;
    case HL_MY_TYPE_LONG:
    case HL_MY_TYPE_INT24:
        out->kind = HL_MY_VAL_INT; out->v.i = (int32_t)hl_my_get_u32(rc); return;
    case HL_MY_TYPE_LONGLONG:
        out->kind = HL_MY_VAL_INT; out->v.i = (int64_t)hl_my_get_u64(rc); return;
    case HL_MY_TYPE_FLOAT: {
        uint32_t bits = hl_my_get_u32(rc);
        float ff; memcpy(&ff, &bits, sizeof ff);
        out->kind = HL_MY_VAL_DOUBLE; out->v.d = (double)ff; return;
    }
    case HL_MY_TYPE_DOUBLE: {
        uint64_t bits = hl_my_get_u64(rc);
        double dd; memcpy(&dd, &bits, sizeof dd);
        out->kind = HL_MY_VAL_DOUBLE; out->v.d = dd; return;
    }
    case HL_MY_TYPE_DATE:
    case HL_MY_TYPE_DATETIME:
    case HL_MY_TYPE_TIMESTAMP: {
        /* Binary temporal: 1 length byte, then 0/4/7/11 bytes of fields.
         * Format to an ISO-8601-ish string into the value's backing buffer. */
        uint8_t len = hl_my_get_u8(rc);
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0; uint32_t us = 0;
        if (len >= 4)  { y = hl_my_get_u16(rc); mo = hl_my_get_u8(rc); d = hl_my_get_u8(rc); }
        if (len >= 7)  { h = hl_my_get_u8(rc); mi = hl_my_get_u8(rc); se = hl_my_get_u8(rc); }
        if (len >= 11) { us = hl_my_get_u32(rc); }
        if (len == 0)
            snprintf(out->tbuf, sizeof out->tbuf, "0000-00-00 00:00:00");
        else if (len == 4)
            snprintf(out->tbuf, sizeof out->tbuf, "%04d-%02d-%02d", y, mo, d);
        else if (us)
            snprintf(out->tbuf, sizeof out->tbuf,
                     "%04d-%02d-%02d %02d:%02d:%02d.%06u", y, mo, d, h, mi, se, us);
        else
            snprintf(out->tbuf, sizeof out->tbuf,
                     "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, se);
        out->kind = HL_MY_VAL_STR;
        out->v.s.ptr = out->tbuf; out->v.s.len = strlen(out->tbuf);
        return;
    }
    case HL_MY_TYPE_TIME: {
        /* Binary TIME: 1 length byte, then 0/8/12 bytes. */
        uint8_t len = hl_my_get_u8(rc);
        int neg = 0, h = 0, mi = 0, se = 0; uint32_t days = 0, us = 0;
        if (len >= 8)  { neg = hl_my_get_u8(rc); days = hl_my_get_u32(rc);
                         h = hl_my_get_u8(rc); mi = hl_my_get_u8(rc); se = hl_my_get_u8(rc); }
        if (len >= 12) { us = hl_my_get_u32(rc); }
        int hours = (int)(days * 24) + h;
        if (len == 0)
            snprintf(out->tbuf, sizeof out->tbuf, "00:00:00");
        else if (us)
            snprintf(out->tbuf, sizeof out->tbuf, "%s%02d:%02d:%02d.%06u",
                     neg ? "-" : "", hours, mi, se, us);
        else
            snprintf(out->tbuf, sizeof out->tbuf, "%s%02d:%02d:%02d",
                     neg ? "-" : "", hours, mi, se);
        out->kind = HL_MY_VAL_STR;
        out->v.s.ptr = out->tbuf; out->v.s.len = strlen(out->tbuf);
        return;
    }
    default: {   /* string / blob / decimal / enum / set / bit: length-encoded bytes */
        size_t l = 0;
        const uint8_t *s = hl_my_get_lenenc_str(rc, &l);
        out->kind = HL_MY_VAL_STR;
        out->v.s.ptr = (const char *)s;
        out->v.s.len = l;
        return;
    }
    }
}

/* Serialize the bound parameters into a COM_STMT_EXECUTE packet body. */
static int build_execute(HlMyWriter *w, uint32_t stmt_id,
                         const HlMyParam *params, int nparams)
{
    hl_my_put_u8(w, HL_MY_COM_STMT_EXECUTE);
    hl_my_put_u32(w, stmt_id);
    hl_my_put_u8(w, HL_MY_STMT_CURSOR_NONE);
    hl_my_put_u32(w, HL_MY_STMT_ITERATIONS);
    if (nparams <= 0) return 0;

    size_t nb = ((size_t)nparams + 7) / 8;
    uint8_t *bitmap = calloc(nb, 1);
    if (!bitmap) return -1;
    for (int i = 0; i < nparams; i++)
        if (params[i].is_null) bitmap[i >> 3] |= (uint8_t)(1u << (i & 7));
    hl_my_put_bytes(w, bitmap, nb);
    free(bitmap);

    hl_my_put_u8(w, HL_MY_STMT_NEW_PARAMS_BOUND);
    for (int i = 0; i < nparams; i++) {
        hl_my_put_u8(w, params[i].is_null ? HL_MY_TYPE_NULL : params[i].type);
        hl_my_put_u8(w, 0);                    /* signed (Hull binds signed) */
    }
    for (int i = 0; i < nparams; i++) {
        if (params[i].is_null) continue;
        switch (params[i].type) {
        case HL_MY_TYPE_TINY:
            hl_my_put_u8(w, (uint8_t)params[i].v.i); break;
        case HL_MY_TYPE_LONGLONG:
            hl_my_put_u64(w, (uint64_t)params[i].v.i); break;
        case HL_MY_TYPE_DOUBLE: {
            uint64_t bits; double dd = params[i].v.d;
            memcpy(&bits, &dd, sizeof bits);
            hl_my_put_u64(w, bits); break;
        }
        default:                               /* VAR_STRING / BLOB */
            hl_my_put_lenenc_str(w, params[i].v.s.ptr, params[i].v.s.len);
            break;
        }
    }
    return 0;
}

int hl_my_conn_query_prepared(HlMyConn *conn, const char *sql,
                              const HlMyParam *params, int nparams,
                              HlMyDescCb desc_cb, HlMyBinRowCb row_cb,
                              void *cb_ctx, int64_t *affected)
{
    if (affected) *affected = -1;

    /* 1. COM_STMT_PREPARE (fresh command: sequence restarts at 0). */
    HlMyWriter w; hl_my_writer_init(&w);
    size_t m = hl_my_packet_begin(&w, 0);
    hl_my_put_u8(&w, HL_MY_COM_STMT_PREPARE);
    hl_my_put_bytes(&w, sql, strlen(sql));
    hl_my_packet_end(&w, m);
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_my_writer_free(&w);
    if (se) { conn_set_err(conn, "failed to send prepare"); return -1; }

    HlMyFrame f;
    if (conn_next_frame(conn, &f) != 0) return -1;
    if (f.body_len > 0 && f.body[0] == HL_MY_PKT_ERR) {
        HlMyErr e;
        if (hl_my_parse_err(&f, 1, &e) == 0 && e.message_len)
            snprintf(conn->errmsg, sizeof conn->errmsg, "prepare failed: %.*s",
                     (int)e.message_len, e.message);
        else conn_set_err(conn, "prepare failed");
        return -1;
    }
    HlMyPrepareOk prep;
    if (hl_my_parse_prepare_ok(&f, &prep) != 0) {
        conn_set_err(conn, "malformed prepare response"); return -1;
    }

    /* The statement is live from here: every exit must close it. */
    char       **names  = NULL;
    HlMyField   *fields = NULL;
    HlMyVal     *vals   = NULL;
    int          ncols  = 0;
    int          ret    = -1;

    /* 2. Drain the PREPARE_OK parameter- and column-definition blocks; the
     *    EXECUTE response re-sends the column metadata we decode against. */
    if (drain_defs(conn, prep.num_params) != 0) goto close_stmt;
    if (drain_defs(conn, prep.num_columns) != 0) goto close_stmt;

    /* 3. COM_STMT_EXECUTE with the bound parameters. */
    hl_my_writer_init(&w);
    m = hl_my_packet_begin(&w, 0);
    int be = build_execute(&w, prep.statement_id, params, nparams);
    hl_my_packet_end(&w, m);
    se = be || w.err || conn_send(conn, w.buf, w.len);
    hl_my_writer_free(&w);
    if (se) { conn_set_err(conn, "failed to send execute"); goto close_stmt; }

    /* 4. EXECUTE response: OK (no result), ERR, or a binary result set. */
    if (conn_next_frame(conn, &f) != 0) goto close_stmt;
    if (f.body_len == 0) { conn_set_err(conn, "empty execute response"); goto close_stmt; }
    uint8_t hdr = f.body[0];

    if (hdr == HL_MY_PKT_OK) {
        HlMyOk ok;
        if (hl_my_parse_ok(&f, &ok) == 0) {
            conn->last_insert_id = ok.last_insert_id;
            if (affected) *affected = (int64_t)ok.affected_rows;
        }
        ret = 0;
        goto close_stmt;
    }
    if (hdr == HL_MY_PKT_ERR) {
        HlMyErr e;
        if (hl_my_parse_err(&f, 1, &e) == 0 && e.message_len)
            snprintf(conn->errmsg, sizeof conn->errmsg, "execute failed: %.*s",
                     (int)e.message_len, e.message);
        else conn_set_err(conn, "execute failed");
        goto close_stmt;
    }

    /* Column count (lenenc), then the column definitions. */
    HlMyCursor cc; hl_my_cursor_init(&cc, &f);
    uint64_t ncols64 = hl_my_get_lenenc_int(&cc, NULL);
    if (hl_my_cursor_err(&cc) || ncols64 == 0 || ncols64 > HL_MY_MAX_COLUMNS) {
        conn_set_err(conn, "malformed column count"); goto close_stmt;
    }
    ncols = (int)ncols64;

    names  = calloc((size_t)ncols, sizeof *names);
    fields = calloc((size_t)ncols, sizeof *fields);
    vals   = calloc((size_t)ncols, sizeof *vals);
    if (!names || !fields || !vals) { conn_set_err(conn, "out of memory"); goto close_stmt; }

    for (int i = 0; i < ncols; i++) {
        if (conn_next_frame(conn, &f) != 0) goto close_stmt;
        HlMyColumn col;
        if (hl_my_parse_column_def(&f, &col) != 0) {
            conn_set_err(conn, "malformed column definition"); goto close_stmt;
        }
        names[i] = malloc(col.name_len + 1);
        if (!names[i]) { conn_set_err(conn, "out of memory"); goto close_stmt; }
        memcpy(names[i], col.name, col.name_len);
        names[i][col.name_len] = '\0';
        fields[i].name = names[i];
        fields[i].type = col.type;
    }

    if (conn_next_frame(conn, &f) != 0) goto close_stmt;   /* EOF after col defs */
    if (desc_cb) desc_cb(cb_ctx, fields, ncols);

    /* 5. Binary result rows until the terminating EOF. */
    int stop = 0;
    for (;;) {
        if (conn_next_frame(conn, &f) != 0) goto close_stmt;
        if (f.body_len > 0 && f.body[0] == HL_MY_PKT_EOF && f.body_len < HL_MY_EOF_MAX_LEN)
            break;                                          /* end of rows */
        if (f.body_len > 0 && f.body[0] == HL_MY_PKT_ERR) {
            HlMyErr e;
            if (hl_my_parse_err(&f, 1, &e) == 0 && e.message_len)
                snprintf(conn->errmsg, sizeof conn->errmsg, "execute failed: %.*s",
                         (int)e.message_len, e.message);
            else conn_set_err(conn, "execute failed mid-result");
            goto close_stmt;
        }
        HlMyCursor rc; hl_my_cursor_init(&rc, &f);
        (void)hl_my_get_u8(&rc);                            /* row header 0x00 */
        size_t nbm = ((size_t)ncols + 7 + HL_MY_STMT_ROW_NULL_OFFSET) / 8;
        const uint8_t *bitmap = hl_my_get_bytes(&rc, nbm);
        if (!bitmap) { conn_set_err(conn, "malformed binary row"); goto close_stmt; }
        for (int i = 0; i < ncols; i++) {
            int bit = i + HL_MY_STMT_ROW_NULL_OFFSET;
            if (bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7)))
                vals[i].kind = HL_MY_VAL_NULL;
            else
                decode_binary_value(&rc, fields[i].type, &vals[i]);
        }
        if (hl_my_cursor_err(&rc)) { conn_set_err(conn, "malformed binary row"); goto close_stmt; }
        if (row_cb && !stop && row_cb(cb_ctx, vals, ncols) != 0)
            stop = 1;                                       /* keep draining to EOF */
    }
    ret = 0;

close_stmt:
    free_names(names, ncols);
    free(fields);
    free(vals);
    /* COM_STMT_CLOSE (fresh command, no response). Best-effort. */
    hl_my_writer_init(&w);
    m = hl_my_packet_begin(&w, 0);
    hl_my_put_u8(&w, HL_MY_COM_STMT_CLOSE);
    hl_my_put_u32(&w, prep.statement_id);
    hl_my_packet_end(&w, m);
    if (!w.err) (void)conn_send(conn, w.buf, w.len);
    hl_my_writer_free(&w);
    return ret;
}

/* ── Multi-statement scripts (migrations / DDL) ───────────────────── */

/* Read one statement's complete response (OK or a full result set, rows
 * discarded), reporting via *more whether another result set follows. */
static int drain_one_result(HlMyConn *conn, int *more)
{
    *more = 0;
    HlMyFrame f;
    if (conn_next_frame(conn, &f) != 0) return -1;
    if (f.body_len == 0) { conn_set_err(conn, "empty script response"); return -1; }
    uint8_t hdr = f.body[0];

    if (hdr == HL_MY_PKT_OK) {
        HlMyOk ok;
        if (hl_my_parse_ok(&f, &ok) == 0) {
            conn->last_insert_id = ok.last_insert_id;
            *more = (ok.status_flags & HL_MY_SERVER_MORE_RESULTS) != 0;
        }
        return 0;
    }
    if (hdr == HL_MY_PKT_ERR) {
        HlMyErr e;
        if (hl_my_parse_err(&f, 1, &e) == 0 && e.message_len)
            snprintf(conn->errmsg, sizeof conn->errmsg, "script failed: %.*s",
                     (int)e.message_len, e.message);
        else conn_set_err(conn, "script failed");
        return -1;
    }
    if (hdr == HL_MY_PKT_LOCAL_INFILE) {
        conn_set_err(conn, "LOCAL INFILE responses are not supported"); return -1;
    }

    /* Result set: column count, then col defs + EOF, then rows to a final EOF
     * whose status flags carry MORE_RESULTS. */
    HlMyCursor cc; hl_my_cursor_init(&cc, &f);
    uint64_t ncols = hl_my_get_lenenc_int(&cc, NULL);
    if (hl_my_cursor_err(&cc) || ncols == 0 || ncols > HL_MY_MAX_COLUMNS) {
        conn_set_err(conn, "malformed column count"); return -1;
    }
    for (uint64_t i = 0; i < ncols; i++)
        if (conn_next_frame(conn, &f) != 0) return -1;   /* column defs */
    if (conn_next_frame(conn, &f) != 0) return -1;       /* EOF after col defs */
    for (;;) {
        if (conn_next_frame(conn, &f) != 0) return -1;
        if (f.body_len > 0 && f.body[0] == HL_MY_PKT_EOF && f.body_len < HL_MY_EOF_MAX_LEN) {
            HlMyCursor ec; hl_my_cursor_init(&ec, &f);
            (void)hl_my_get_u8(&ec);                     /* 0xFE */
            (void)hl_my_get_u16(&ec);                    /* warnings */
            uint16_t status = hl_my_get_u16(&ec);
            *more = (status & HL_MY_SERVER_MORE_RESULTS) != 0;
            return 0;
        }
        if (f.body_len > 0 && f.body[0] == HL_MY_PKT_ERR) {
            HlMyErr e;
            if (hl_my_parse_err(&f, 1, &e) == 0 && e.message_len)
                snprintf(conn->errmsg, sizeof conn->errmsg, "script failed: %.*s",
                         (int)e.message_len, e.message);
            else conn_set_err(conn, "script failed mid-result");
            return -1;
        }
        /* otherwise a data row: discarded */
    }
}

int hl_my_conn_exec_multi(HlMyConn *conn, const char *sql)
{
    HlMyWriter w; hl_my_writer_init(&w);
    size_t m = hl_my_packet_begin(&w, 0);
    hl_my_put_u8(&w, HL_MY_COM_QUERY);
    hl_my_put_bytes(&w, sql, strlen(sql));
    hl_my_packet_end(&w, m);
    int se = w.err || conn_send(conn, w.buf, w.len);
    hl_my_writer_free(&w);
    if (se) { conn_set_err(conn, "failed to send script"); return -1; }

    int more = 1;
    while (more) {
        if (drain_one_result(conn, &more) != 0) return -1;
    }
    return 0;
}
#endif
