/*
 * cap/valkey_conn.c: Valkey/Redis DSN parser (+ connection in a later part).
 *
 * The DSN parser is pure and fuzzed (fuzz/fuzz_valkey_dsn.c): every field is
 * bounded, percent-escapes are decoded, oversized input is rejected. See
 * valkey_conn.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/valkey_conn.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        querylen -= (size_t)(amp + 1 - query);
        query = amp + 1;
    }

    return 0;
#undef FAIL
}
