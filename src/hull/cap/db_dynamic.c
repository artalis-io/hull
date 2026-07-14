/*
 * cap/db_dynamic.c: db.open(dsn) validation + caller-owned open. See the header.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db_dynamic.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/fs.h"
#include "hull/manifest.h"
#include "hull/host_match.h"

#include <stdlib.h>
#include <string.h>

/* Process-wide cap on concurrent dynamic connections (fd-exhaustion backstop).
 * db.open / close run on the event-loop thread, so a plain int is race-free. */
#define HL_DB_DYNAMIC_MAX 16
static int g_dynamic_open_count;

int hl_db_dynamic_open_count(void) { return g_dynamic_open_count; }

/* Lowercased scheme (the text before "://") into @p buf; "" when the DSN has no
 * "://" (a bare path or ":memory:"). */
static void dsn_scheme(const char *dsn, char *buf, size_t bufsz)
{
    buf[0] = '\0';
    const char *sep = strstr(dsn, "://");
    if (!sep) return;
    size_t n = (size_t)(sep - dsn);
    if (n == 0 || n >= bufsz) return;
    for (size_t i = 0; i < n; i++) {
        char c = dsn[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    buf[n] = '\0';
}

/* File-based backends key off the local filesystem, so they are gated by the fs
 * allowlist, not the host allowlist. A scheme-less DSN implies SQLite. */
static int scheme_is_file(const char *scheme)
{
    return scheme[0] == '\0' ||
           strcmp(scheme, "sqlite") == 0 ||
           strcmp(scheme, "file")   == 0 ||
           strcmp(scheme, "duckdb") == 0;
}

/* Extract the host from a network DSN authority
 * (scheme://[user[:pass]@]host[:port]/...). IPv6 literals "[::1]" are returned
 * without the brackets (so host_match's inet_pton sees a bare address). Returns
 * 1 + host in @p buf, or 0 if no host is present. */
static int dsn_host(const char *dsn, char *buf, size_t bufsz)
{
    const char *sep = strstr(dsn, "://");
    if (!sep) return 0;
    const char *p   = sep + 3;
    const char *end = p + strcspn(p, "/?#");   /* authority ends at path/query */

    const char *at = NULL;                     /* last '@' delimits userinfo */
    for (const char *q = p; q < end; q++)
        if (*q == '@') at = q;
    const char *host = at ? at + 1 : p;

    const char *hend;
    if (host < end && *host == '[') {          /* IPv6 literal */
        const char *close = memchr(host, ']', (size_t)(end - host));
        if (!close) return 0;
        host++;
        hend = close;
    } else {
        hend = host;
        while (hend < end && *hend != ':')     /* stop at :port */
            hend++;
    }

    size_t n = (size_t)(hend - host);
    if (n == 0 || n >= bufsz) return 0;
    memcpy(buf, host, n);
    buf[n] = '\0';
    return 1;
}

/* Match @p host against the policy's host patterns, resolving "$VAR" entries
 * from the environment. */
static int host_allowed(const HlManifestDbDynamic *policy, const char *host)
{
    char var[128];
    for (int i = 0; i < policy->host_count; i++) {
        const char *pat = policy->hosts[i];
        if (hl_manifest_env_ref(pat, var, sizeof var)) {
            const char *v = getenv(var);
            if (v && v[0] && hl_host_match(v, host)) return 1;
        } else if (hl_host_match(pat, host)) {
            return 1;
        }
    }
    return 0;
}

HlDbHandle *hl_db_dynamic_open(const char *dsn,
                               const HlManifestDbDynamic *policy,
                               const HlFsConfig *fs_cfg,
                               const char **err)
{
    if (err) *err = NULL;
    if (!dsn || !dsn[0]) {
        if (err) *err = "db.open: empty DSN";
        return NULL;
    }
    if (!policy || !policy->declared) {
        if (err) *err = "db.open requires a databases.dynamic policy in the manifest";
        return NULL;
    }
    if (g_dynamic_open_count >= HL_DB_DYNAMIC_MAX) {
        if (err) *err = "db.open: too many open dynamic connections (close some first)";
        return NULL;
    }

    char scheme[24];
    dsn_scheme(dsn, scheme, sizeof scheme);

    /* Scheme must be allowlisted. A bare path (no scheme) implies "sqlite". */
    const char *want = scheme[0] ? scheme : "sqlite";
    int scheme_ok = 0;
    for (int i = 0; i < policy->scheme_count; i++)
        if (strcmp(policy->schemes[i], want) == 0) { scheme_ok = 1; break; }
    if (!scheme_ok) {
        if (err) *err = "db.open: DSN scheme not in databases.dynamic.schemes";
        return NULL;
    }

    /* Backend must be compiled + selectable for this DSN. */
    const char *sel_err = NULL;
    const HlDbBackend *be = hl_db_backend_select(dsn, &sel_err);
    if (!be) {
        if (err) *err = sel_err ? sel_err : "db.open: no backend for DSN";
        return NULL;
    }

    if (scheme_is_file(scheme)) {
        /* File backend: the path (DSN minus a "sqlite://" prefix) must pass the
         * fs sandbox. ":memory:" opens no file. */
        const char *path = dsn;
        if (strncmp(path, "sqlite://", 9) == 0) path += 9;
        if (strcmp(path, ":memory:") != 0) {
            if (!fs_cfg) {
                if (err) *err = "db.open: a file DSN needs a filesystem-scoped app";
                return NULL;
            }
            const char *fe = NULL;
            if (hl_cap_fs_validate(fs_cfg, path, &fe) != 0) {
                if (err) *err = fe ? fe : "db.open: file path not allowed by manifest.fs";
                return NULL;
            }
        }
    } else {
        /* Network backend: host must match databases.dynamic.hosts. */
        char host[256];
        if (!dsn_host(dsn, host, sizeof host)) {
            if (err) *err = "db.open: could not parse host from DSN";
            return NULL;
        }
        if (!host_allowed(policy, host)) {
            if (err) *err = "db.open: host not allowed by databases.dynamic.hosts";
            return NULL;
        }
    }

    HlDbHandle *h = calloc(1, sizeof *h);
    if (!h) {
        if (err) *err = "db.open: out of memory";
        return NULL;
    }
    h->backend = be;
    if (be->open(&h->ctx, dsn, NULL) != 0) {
        if (err) *err = "db.open: connection failed";
        free(h);
        return NULL;
    }
    g_dynamic_open_count++;
    return h;
}

void hl_db_dynamic_close(HlDbHandle *h)
{
    if (!h) return;
    if (h->backend)
        h->backend->close(h);
    free(h);
    if (g_dynamic_open_count > 0)
        g_dynamic_open_count--;
}

#endif /* HL_ENABLE_DB */
