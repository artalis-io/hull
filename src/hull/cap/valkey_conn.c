/*
 * cap/valkey_conn.c: Valkey/Redis DSN parser + RESP connection transport.
 *
 * The DSN parser is pure and fuzzed (fuzz/fuzz_valkey_dsn.c): every field is
 * bounded, percent-escapes decoded, oversized input rejected. The connection
 * (blocking TCP + optional rediss TLS via shared/tls_client.c) runs the
 * HELLO 3 / AUTH handshake with a RESP2 fallback, then SELECT, and issues
 * commands via the RESP codec (cap/respwire.c). Replies borrow into a bounded
 * receive buffer + a non-moving reply arena, valid until the next command.
 * Covered by test_valkey_conn (socketpair). -DHL_VALKEY_NO_TLS drops the TLS
 * branch for the DSN-only test/fuzzer. See valkey_conn.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/valkey_conn.h"
#include "hull/cap/respwire.h"
#include "hull/utils/alloc.h"
#include "sh_arena.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>   /* ssize_t (io_send/io_recv) */

#ifndef HL_VALKEY_NO_TLS
#include "hull/cap/db_transport.h"   /* shared Keel-v3 byte transport (bytes + TLS) */
#include "hull/shared/tls_client.h"
#endif

/* Cap the receive buffer so a hostile server can't force unbounded growth. */
#ifndef HL_VALKEY_MAX_REPLY
#define HL_VALKEY_MAX_REPLY (64u * 1024u * 1024u)
#endif

/* Fixed capacity of the per-connection reply arena: it holds decoded aggregate
 * item arrays (payload bytes borrow into rbuf), reset per reply. A reply whose
 * item metadata exceeds this is rejected - a natural bound on hostile
 * aggregates, since sh_arena_alloc returns NULL when full. */
#ifndef HL_VALKEY_REPLY_ARENA_CAP
#define HL_VALKEY_REPLY_ARENA_CAP (256u * 1024u)
#endif

/* Initial receive-buffer size; grows (via the allocator) up to MAX_REPLY. */
#define HL_VALKEY_RBUF_INIT 8192u
/* Error-message buffer size (matches the pg/mysql wire-client convention). */
#define HL_VALKEY_ERRMSG    256u

void hl_valkey_dsn_scrub(HlValkeyDsn *dsn) {
    if (dsn) memset(dsn->password, 0, sizeof dsn->password);
}

static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode [s, s+n) into out (NUL-terminated). -1 if it won't fit or a
 * `%XX` escape is malformed. */
static int pct_decode(const char *s, size_t n, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (o + 1 >= outsz) return -1;
        if (s[i] == '%') {
            if (i + 2 >= n) return -1;
            int hi = hexval((unsigned char)s[i + 1]);
            int lo = hexval((unsigned char)s[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            out[o++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
    return 0;
}

/* Copy [s, s+n) verbatim into out (NUL-terminated). -1 if it won't fit. */
static int copy_bounded(const char *s, size_t n, char *out, size_t outsz) {
    if (n + 1 > outsz) return -1;
    memcpy(out, s, n);
    out[n] = '\0';
    return 0;
}

static int ci_eq(const char *a, size_t n, const char *lit) {
    if (strlen(lit) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != (unsigned char)lit[i]) return 0;
    return 1;
}

int hl_valkey_dsn_parse(const char *dsn, HlValkeyDsn *out, char *errbuf, size_t errlen) {
#define FAIL(msg) do { if (errbuf && errlen) snprintf(errbuf, errlen, "%s", (msg)); return -1; } while (0)
    if (!dsn || !out) FAIL("valkey dsn: null");
    memset(out, 0, sizeof *out);
    snprintf(out->port, sizeof out->port, "6379");
    snprintf(out->dbindex, sizeof out->dbindex, "0");
    out->connect_timeout_ms = 5000;

    const char *sep = strstr(dsn, "://");
    if (!sep) FAIL("valkey dsn: missing scheme://");
    size_t schlen = (size_t)(sep - dsn);
    if      (ci_eq(dsn, schlen, "redis"))   { out->tls = 0; }
    else if (ci_eq(dsn, schlen, "valkey"))  { out->tls = 0; }
    else if (ci_eq(dsn, schlen, "rediss"))  { out->tls = 1; out->verify = 1; }
    else if (ci_eq(dsn, schlen, "valkeys")) { out->tls = 1; out->verify = 1; }
    else FAIL("valkey dsn: scheme must be redis/rediss/valkey/valkeys");

    const char *rest = sep + 3;
    size_t restlen = strlen(rest);

    /* Split query (?...) off the end. */
    const char *query = NULL;
    size_t querylen = 0;
    const char *q = memchr(rest, '?', restlen);
    if (q) { query = q + 1; querylen = (size_t)(rest + restlen - query); restlen = (size_t)(q - rest); }

    /* Split path (/db) off (query already excluded from restlen above). */
    const char *path = NULL;
    size_t pathlen = 0;
    const char *slash = memchr(rest, '/', restlen);
    if (slash) {
        path = slash + 1;
        pathlen = (size_t)(rest + restlen - path);
        restlen = (size_t)(slash - rest);
    }

    /* authority = [userinfo@]host[:port]; find the LAST '@'. */
    const char *authority = rest;
    size_t authlen = restlen;
    const char *at = NULL;
    for (size_t i = 0; i < authlen; i++) if (authority[i] == '@') at = authority + i;
    const char *hostport = authority;
    size_t hplen = authlen;
    if (at) {
        size_t uilen = (size_t)(at - authority);
        const char *colon = memchr(authority, ':', uilen);
        if (colon) {
            if (pct_decode(authority, (size_t)(colon - authority), out->username, sizeof out->username) != 0)
                FAIL("valkey dsn: username too long");
            if (pct_decode(colon + 1, (size_t)(at - (colon + 1)), out->password, sizeof out->password) != 0)
                FAIL("valkey dsn: password too long");
        } else {
            if (pct_decode(authority, uilen, out->username, sizeof out->username) != 0)
                FAIL("valkey dsn: username too long");
        }
        hostport = at + 1;
        hplen = (size_t)(authority + authlen - hostport);
    }

    /* host[:port], with [ipv6] support. */
    if (hplen > 0 && hostport[0] == '[') {
        const char *close = memchr(hostport, ']', hplen);
        if (!close) FAIL("valkey dsn: unterminated IPv6 literal");
        if (copy_bounded(hostport + 1, (size_t)(close - (hostport + 1)), out->host, sizeof out->host) != 0)
            FAIL("valkey dsn: host too long");
        const char *after = close + 1;
        size_t afterlen = (size_t)(hostport + hplen - after);
        if (afterlen > 0) {
            if (after[0] != ':') FAIL("valkey dsn: junk after IPv6 host");
            if (copy_bounded(after + 1, afterlen - 1, out->port, sizeof out->port) != 0)
                FAIL("valkey dsn: port too long");
        }
    } else {
        const char *colon = memchr(hostport, ':', hplen);
        if (colon) {
            if (copy_bounded(hostport, (size_t)(colon - hostport), out->host, sizeof out->host) != 0)
                FAIL("valkey dsn: host too long");
            if (copy_bounded(colon + 1, (size_t)(hostport + hplen - (colon + 1)), out->port, sizeof out->port) != 0)
                FAIL("valkey dsn: port too long");
        } else {
            if (copy_bounded(hostport, hplen, out->host, sizeof out->host) != 0)
                FAIL("valkey dsn: host too long");
        }
    }
    if (out->host[0] == '\0') FAIL("valkey dsn: missing host");

    /* Port must be numeric (1..65535). */
    for (const char *pc = out->port; *pc; pc++) if (!isdigit((unsigned char)*pc)) FAIL("valkey dsn: non-numeric port");
    long portn = atol(out->port);
    if (portn < 1 || portn > 65535) FAIL("valkey dsn: port out of range");

    /* Path -> DB index (digits). */
    if (path && pathlen > 0) {
        for (size_t i = 0; i < pathlen; i++) if (!isdigit((unsigned char)path[i])) FAIL("valkey dsn: db index must be digits");
        if (copy_bounded(path, pathlen, out->dbindex, sizeof out->dbindex) != 0) FAIL("valkey dsn: db index too long");
    }

    /* Query opts: connect_timeout (ms), sslmode. */
    while (query && querylen > 0) {
        const char *amp = memchr(query, '&', querylen);
        size_t pairlen = amp ? (size_t)(amp - query) : querylen;
        const char *eq = memchr(query, '=', pairlen);
        if (eq) {
            size_t klen = (size_t)(eq - query);
            const char *val = eq + 1;
            size_t vlen = pairlen - klen - 1;
            if (ci_eq(query, klen, "connect_timeout")) {
                char tb[16];
                if (copy_bounded(val, vlen, tb, sizeof tb) == 0) {
                    long ms = atol(tb);
                    if (ms >= 0 && ms <= 600000) out->connect_timeout_ms = (int)ms;
                }
            } else if (ci_eq(query, klen, "sslmode")) {
                if (ci_eq(val, vlen, "verify-full"))       out->verify = 1;
                else if (ci_eq(val, vlen, "require"))       out->verify = 0;
                else if (ci_eq(val, vlen, "disable") || ci_eq(val, vlen, "none")) out->verify = 0;
            }
        }
        if (!amp) break;
        /* Advance past "<pair>&". amp - query == pairlen, so the step is
         * pairlen + 1; using pairlen (already computed) avoids a pointer
         * subtraction that static analysis flags around the loop's `query`
         * null-check. */
        query = amp + 1;
        querylen -= pairlen + 1;
    }

    return 0;
#undef FAIL
}

/* ── Connection ───────────────────────────────────────────────────────────
 *
 * The connection owns a fixed-capacity sh_arena (allocated through Hull's
 * pluggable HlAllocator) for decoded aggregate item arrays, reset per reply.
 * ALL memory - the handle, the receive buffer, the arena, AND the byte transport
 * (docs/valkey_keel_transport_slice5.md D6) - comes from c->alloc, so an embedder's
 * allocator and limits apply. */

/* The whole connection layer rides the shared Keel-v3 byte transport
 * (cap/db_transport.c) and therefore needs Keel; it is compiled out under
 * HL_VALKEY_NO_TLS (the DSN-parser-only build for test_valkey_dsn / fuzz_valkey_dsn),
 * mirroring MySQL's HL_MY_NO_TLS. The DSN parser above stays Keel-free. */
#ifndef HL_VALKEY_NO_TLS

struct HlValkeyConn {
    HlAllocator *alloc;
    HlDbTransport *transport;   /* shared byte transport (bytes + optional TLS) */
    uint8_t *rbuf;
    size_t   rlen, rcap, consumed;
    SHArena *arena;          /* reply aggregate items; reset per reply */
    char     errmsg[HL_VALKEY_ERRMSG];
    int      resp3;
};

HlAllocator *hl_valkey_conn_alloc(HlValkeyConn *c) { return c ? c->alloc : NULL; }

static void set_err(HlValkeyConn *c, const char *msg) {
    snprintf(c->errmsg, sizeof c->errmsg, "%s", msg);
}

/* HlRespAlloc over the connection's reply arena. Returns NULL when the fixed
 * arena is full, which makes the parser reject the reply - a natural bound on
 * a hostile aggregate. */
static void *reply_alloc(void *ctx, size_t n) {
    HlValkeyConn *c = (HlValkeyConn *)ctx;
    return sh_arena_alloc(c->arena, n);
}

/* All bytes ride the shared Keel-v3 byte transport, which is EINTR-retried,
 * SIGPIPE-suppressed, and (when a TLS session is attached) TLS-aware internally -
 * so these are thin single-op wrappers, one per direction. */
static ssize_t io_send(HlValkeyConn *c, const uint8_t *buf, size_t len) {
    return hl_db_transport_send(c->transport, buf, len);
}

static ssize_t io_recv(HlValkeyConn *c, uint8_t *buf, size_t len) {
    return hl_db_transport_recv(c->transport, buf, len);
}

static int conn_send(HlValkeyConn *c, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = io_send(c, buf + sent, len - sent);
        if (n <= 0) { set_err(c, "socket write failed"); return -1; }
        sent += (size_t)n;
    }
    return 0;
}

/* Read exactly one reply into *out (borrows into rbuf + the reply arena, valid
 * until the next read). Returns 0 / -1. */
static int read_reply(HlValkeyConn *c, HlRespValue *out) {
    if (c->consumed > 0) {
        memmove(c->rbuf, c->rbuf + c->consumed, c->rlen - c->consumed);
        c->rlen -= c->consumed;
        c->consumed = 0;
    }
    for (;;) {
        sh_arena_reset(c->arena);
        size_t consumed = 0;
        HlRespResult r = hl_resp_parse(c->rbuf, c->rlen, &consumed, out, reply_alloc, c);
        if (r == HL_RESP_OK) { c->consumed = consumed; return 0; }
        if (r == HL_RESP_PARSE_ERR) { set_err(c, "malformed reply from server"); return -1; }
        if (c->rlen == c->rcap) {                 /* NEED_MORE: grow bounded */
            size_t ncap = c->rcap ? c->rcap * 2 : HL_VALKEY_RBUF_INIT;
            if (ncap > HL_VALKEY_MAX_REPLY) { set_err(c, "server reply exceeds limit"); return -1; }
            uint8_t *nb = hl_alloc_realloc(c->alloc, c->rbuf, c->rcap, ncap);
            if (!nb) { set_err(c, "out of memory"); return -1; }
            c->rbuf = nb; c->rcap = ncap;
        }
        ssize_t n = io_recv(c, c->rbuf + c->rlen, c->rcap - c->rlen);
        if (n <= 0) { set_err(c, "connection closed by server"); return -1; }
        c->rlen += (size_t)n;
    }
}

/* Send a command built with hl_resp_cmd_* and read its reply. */
int hl_valkey_command(HlValkeyConn *c, const HlRespWriter *cmd, HlRespValue *out) {
    if (cmd->err) { set_err(c, "command encode failed"); return -1; }
    if (conn_send(c, cmd->buf, cmd->len) != 0) return -1;
    return read_reply(c, out);
}

static int reply_is_err(const HlRespValue *v) { return v && v->type == HL_RESP_ERR; }

/* Bounded substring search (memmem is a GNU/BSD extension; avoid it). */
static int mem_has(const char *hay, size_t hn, const char *needle) {
    size_t nn = strlen(needle);
    if (nn == 0 || nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, needle, nn) == 0) return 1;
    return 0;
}

/* Does a server error look like "HELLO not understood" (pre-6 server) vs a real
 * auth/other failure? */
static int err_is_unknown_cmd(const HlRespValue *v) {
    if (!reply_is_err(v)) return 0;
    return mem_has(v->str.p, v->str.len, "unknown command") ||
           mem_has(v->str.p, v->str.len, "unknown subcommand");
}

/* HELLO 3 [AUTH user pass]; on an unknown-command error fall back to RESP2 +
 * a legacy AUTH. Returns 0 / -1. */
static int handshake(HlValkeyConn *c, const HlValkeyDsn *dsn) {
    int have_pass = dsn->password[0] != '\0';
    const char *user = dsn->username[0] ? dsn->username : "default";

    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, have_pass ? 5 : 2);
    hl_resp_cmd_arg_cstr(&w, "HELLO");
    hl_resp_cmd_arg_cstr(&w, "3");
    if (have_pass) {
        hl_resp_cmd_arg_cstr(&w, "AUTH");
        hl_resp_cmd_arg_cstr(&w, user);
        hl_resp_cmd_arg(&w, dsn->password, strlen(dsn->password));
    }
    HlRespValue reply;
    int rc = hl_valkey_command(c, &w, &reply);
    hl_resp_writer_free(&w);
    if (rc != 0) return -1;

    if (!reply_is_err(&reply)) { c->resp3 = 1; return 0; }   /* HELLO 3 accepted */
    if (!err_is_unknown_cmd(&reply)) {
        set_err(c, "server rejected HELLO (auth failed?)");
        return -1;
    }
    /* Pre-6 server: RESP2 + legacy AUTH. */
    c->resp3 = 0;
    if (have_pass) {
        hl_resp_writer_init(&w);
        if (dsn->username[0]) {
            hl_resp_cmd_begin(&w, 3);
            hl_resp_cmd_arg_cstr(&w, "AUTH");
            hl_resp_cmd_arg_cstr(&w, dsn->username);
            hl_resp_cmd_arg(&w, dsn->password, strlen(dsn->password));
        } else {
            hl_resp_cmd_begin(&w, 2);
            hl_resp_cmd_arg_cstr(&w, "AUTH");
            hl_resp_cmd_arg(&w, dsn->password, strlen(dsn->password));
        }
        rc = hl_valkey_command(c, &w, &reply);
        hl_resp_writer_free(&w);
        if (rc != 0) return -1;
        if (!hl_resp_is_ok(&reply)) { set_err(c, "AUTH rejected"); return -1; }
    }
    return 0;
}

/* SELECT the DB index if non-zero. */
static int select_db(HlValkeyConn *c, const HlValkeyDsn *dsn) {
    if (dsn->dbindex[0] == '\0' || strcmp(dsn->dbindex, "0") == 0) return 0;
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "SELECT");
    hl_resp_cmd_arg_cstr(&w, dsn->dbindex);
    HlRespValue reply;
    int rc = hl_valkey_command(c, &w, &reply);
    hl_resp_writer_free(&w);
    if (rc != 0) return -1;
    if (!hl_resp_is_ok(&reply)) { set_err(c, "SELECT rejected"); return -1; }
    return 0;
}

static HlValkeyConn *conn_new(HlAllocator *alloc, HlDbTransport *transport) {
    HlValkeyConn *c = (HlValkeyConn *)hl_alloc_calloc(alloc, 1, sizeof *c);
    if (!c) return NULL;
    c->alloc = alloc;
    c->transport = transport;
    c->arena = hl_arena_create(alloc, HL_VALKEY_REPLY_ARENA_CAP);
    if (!c->arena) { hl_alloc_free(alloc, c, sizeof *c); return NULL; }
    return c;
}

/* Adopt an already-connected descriptor (embedder / socketpair-test path). No I/O
 * timeout is installed here: the caller owns the socket and set none, matching the
 * historic hl_valkey_conn_start which never touched SO_*TIMEO (D3). */
int hl_valkey_conn_start(HlValkeyConn **out, int fd, const HlValkeyDsn *dsn,
                         HlAllocator *alloc, char *errbuf, size_t errlen) {
    HlDbTransport *t = hl_db_transport_adopt("valkey", alloc, fd, NULL, errbuf, errlen);
    if (!t) return -1;   /* adopt sets errbuf and consumes fd on a closable failure */
    HlValkeyConn *c = conn_new(alloc, t);
    if (!c) {
        hl_db_transport_close(t);
        if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory");
        return -1;
    }
    if (handshake(c, dsn) != 0 || select_db(c, dsn) != 0) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "%s", c->errmsg);
        hl_valkey_conn_close(c);
        return -1;
    }
    *out = c;
    return 0;
}

int hl_valkey_conn_open(HlValkeyConn **out, const HlValkeyDsn *dsn,
                        HlAllocator *alloc, char *errbuf, size_t errlen) {
    HlDbTransport *t = hl_db_transport_connect("valkey", alloc, dsn->host, dsn->port,
                                               dsn->connect_timeout_ms, NULL, NULL, 0);
    if (!t) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "connect to %s:%s failed", dsn->host, dsn->port);
        return -1;
    }
    /* Preserve the historic bounded blocking recv/send (D3): connect path only,
     * only when a positive timeout is configured (set_io_timeout no-ops on <= 0). */
    hl_db_transport_set_io_timeout(t, dsn->connect_timeout_ms);

    /* Implicit TLS (rediss / valkeys): handshake immediately, before any RESP byte,
     * then hand the session to the transport so every subsequent byte tunnels it. */
    if (dsn->tls) {
        HlTlsClient *tls = hl_tls_client_handshake(hl_db_transport_fd(t), dsn->host,
                                                   dsn->verify, dsn->connect_timeout_ms);
        if (!tls || hl_db_transport_attach_tls(t, tls) != 0) {
            if (tls) hl_tls_client_free(tls);   /* attach rejected: we still own it */
            if (errbuf && errlen) snprintf(errbuf, errlen, "TLS handshake to %s failed", dsn->host);
            hl_db_transport_close(t);
            return -1;
        }
    }

    HlValkeyConn *c = conn_new(alloc, t);
    if (!c) {
        hl_db_transport_close(t);
        if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory");
        return -1;
    }
    if (handshake(c, dsn) != 0 || select_db(c, dsn) != 0) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "%s", c->errmsg);
        hl_valkey_conn_close(c);
        return -1;
    }
    *out = c;
    return 0;
}

const char *hl_valkey_conn_error(HlValkeyConn *c) { return c ? c->errmsg : ""; }
int hl_valkey_conn_is_resp3(HlValkeyConn *c) { return c ? c->resp3 : 0; }

void hl_valkey_conn_close(HlValkeyConn *c) {
    if (!c) return;
    hl_db_transport_close(c->transport);   /* TLS shutdown/free + provider close + free */
    if (c->rbuf) {
        memset(c->rbuf, 0, c->rcap);                 /* scrub residual bytes */
        hl_alloc_free(c->alloc, c->rbuf, c->rcap);
    }
    hl_arena_free(c->alloc, c->arena);
    hl_alloc_free(c->alloc, c, sizeof *c);
}

#endif /* HL_VALKEY_NO_TLS: the transport-backed connection layer */
