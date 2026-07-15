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

/* ── Auth plugins (crypto) ────────────────────────────────────────── */
#ifndef HL_MY_NO_AUTH
#include "hull/cap/crypto.h"

int hl_my_native_password_scramble(const char *password,
                                   const uint8_t scramble[20], uint8_t out[20])
{
    if (!password || !password[0])
        return 0;   /* empty password -> zero-length auth response */

    uint8_t h1[20], h2[20], cat[40], h3[20];
    if (hl_cap_crypto_sha1(password, strlen(password), h1) != 0) return -1;
    if (hl_cap_crypto_sha1(h1, 20, h2) != 0) return -1;
    memcpy(cat, scramble, 20);
    memcpy(cat + 20, h2, 20);
    if (hl_cap_crypto_sha1(cat, 40, h3) != 0) return -1;
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(h1[i] ^ h3[i]);
    return 20;
}
#endif
