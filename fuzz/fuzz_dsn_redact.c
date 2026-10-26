/*
 * fuzz_dsn_redact.c: libFuzzer harness for hl_db_dsn_redact (cap/db_select.c,
 * audits 9 L4 / 10): the form `hull migrate` prints a database DSN in, whose
 * password is often a resolved "$VAR" secret.
 *
 * The first byte picks a mode; the rest are NUL-separated fields.
 *
 * Mode 0 (raw): bounds - exact-size output buffers of every size, NUL inside
 * outsz, the result a prefix of the untruncated one, return value consistent
 * with snprintf (strlen of the untruncated form).
 *
 * Mode 1 (credentials): DSN = scheme "://" user ":" "PW!" pass "@" host
 * rest, for the network schemes Hull ships. The redaction must never contain
 * the password marker "PW!" (user / host / rest have '!' stripped, so only
 * the password can supply it), and when the password and rest hold no
 * delimiter it must be exactly scheme "://" host.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/db_backend.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what, const char *s)
{
    fprintf(stderr, "fuzz_dsn_redact: %s\n  input: \"", what);
    for (const char *p = s ? s : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') fputc(c, stderr);
        else fprintf(stderr, "\\x%02x", c);
    }
    fprintf(stderr, "\"\n");
    abort();
}

static char *redact(const char *dsn, size_t outsz, size_t *ret)
{
    char *out = malloc(outsz ? outsz : 1);
    if (!out) abort();
    out[0] = 'Z';
    *ret = hl_db_dsn_redact(dsn, out, outsz);
    if (outsz == 0) {
        if (out[0] != 'Z') fail("outsz 0 wrote to the buffer", dsn);
        out[0] = '\0';
        return out;
    }
    if (!memchr(out, '\0', outsz)) fail("output not NUL-terminated inside outsz", dsn);
    return out;
}

static void strip(char *s, const char *ban)
{
    char *w = s;
    for (char *p = s; *p; p++) if (!strchr(ban, *p)) *w++ = *p;
    *w = '\0';
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > 4096) return 0;
    char *buf = malloc(size);
    if (!buf) return 0;
    memcpy(buf, data + 1, size - 1);
    buf[size - 1] = '\0';

    if (data[0] % 2 == 0) {
        const char *dsn = buf;
        size_t full_ret;
        char *full = redact(dsn, strlen(dsn) + 32, &full_ret);
        if (full_ret != strlen(full)) fail("return value != strlen(full)", dsn);
        for (size_t sz = 0; sz < 40 && sz <= full_ret + 1; sz++) {
            size_t r;
            char *t = redact(dsn, sz, &r);
            if (sz && r != full_ret) fail("return value depends on outsz", dsn);
            if (strncmp(full, t, strlen(t)) != 0)
                fail("truncated redaction is not a prefix of the full one", dsn);
            free(t);
        }
        free(full);
    } else {
        static const char *const schemes[] = {
            "postgres://", "postgresql://", "mysql://", "mariadb://",
            "redis://", "valkey://", "rediss://", "duckdb://"
        };
        /* fields: user \0 pass \0 host \0 rest */
        char *f[4] = { buf, "", "", "" };
        char *p = buf;
        for (int i = 1; i < 4; i++) {
            char *z = strchr(p, '\0');
            if (z >= buf + size - 1) break;
            p = z + 1;
            f[i] = p;
        }
        char user[1024], pass[1024], host[1024], rest[1024];
        snprintf(user, sizeof user, "%s", f[0]);
        snprintf(pass, sizeof pass, "%s", f[1]);
        snprintf(host, sizeof host, "%s", f[2]);
        snprintf(rest, sizeof rest, "%s", f[3]);
        strip(user, "!@:/?#"); strip(host, "!@/?#"); strip(rest, "!");
        const char *scheme = schemes[(unsigned char)data[0] / 2 % 8];
        if (rest[0] && !strchr("/?#", rest[0])) {   /* keep it off the host */
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "/%s", rest);
            memcpy(rest, tmp, sizeof rest);
        }
        char dsn[4200];
        snprintf(dsn, sizeof dsn, "%s%s:PW!%s@%s%s", scheme, user, pass, host, rest);
        size_t r;
        char *out = redact(dsn, sizeof dsn, &r);
        if (strstr(out, "PW!")) fail("DSN redaction kept the password", dsn);
        if (!strpbrk(pass, "/?#@") && !strchr(rest, '@')) {
            char want[2200];
            snprintf(want, sizeof want, "%.*s%s", (int)strlen(scheme), scheme, host);
            if (strcmp(out, want) != 0)
                fail("DSN redaction is not scheme://host", dsn);
        }
        free(out);
    }
    free(buf);
    return 0;
}
