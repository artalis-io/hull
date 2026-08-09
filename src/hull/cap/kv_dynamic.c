/*
 * cap/kv_dynamic.c: kv.open(dsn) allowlist validation (base-resident).
 *
 * Mirrors cap/db_dynamic.c's DSN gate for the KV path: the scheme must be listed
 * in kv.dynamic.schemes and the host must match kv.dynamic.hosts via the shared
 * host matcher (the same one http / smtp / db use). Unlike db.open there is no
 * file-backed KV scheme, so every KV DSN is a network DSN gated purely by hosts.
 * Fails closed: a NULL / undeclared / empty policy denies every open.
 *
 * This is the enforcement layer; it is base-resident (not gated on
 * HL_ENABLE_VALKEY) so the manifest allowlist is checked even before backend
 * selection, and the denial message is a policy error, not "no backend".
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/kv.h"
#include "hull/manifest.h"
#include "hull/host_match.h"

#include <stdio.h>
#include <string.h>

/* Lowercased scheme (text before "://") into buf; "" when absent. */
static void kv_dsn_scheme(const char *dsn, char *buf, size_t bufsz)
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

/* Host from a network DSN authority (scheme://[user[:pass]@]host[:port]/...).
 * IPv6 "[::1]" is returned without brackets. Returns 1 + host, or 0. */
static int kv_dsn_host(const char *dsn, char *buf, size_t bufsz)
{
    const char *sep = strstr(dsn, "://");
    if (!sep) return 0;
    const char *p   = sep + 3;
    const char *end = p + strcspn(p, "/?#");

    const char *at = NULL;
    for (const char *q = p; q < end; q++)
        if (*q == '@') at = q;
    const char *host = at ? at + 1 : p;

    const char *hend;
    if (host < end && *host == '[') {
        const char *close = memchr(host, ']', (size_t)(end - host));
        if (!close) return 0;
        host++;
        hend = close;
    } else {
        hend = host;
        while (hend < end && *hend != ':')
            hend++;
    }

    size_t n = (size_t)(hend - host);
    if (n == 0 || n >= bufsz) return 0;
    memcpy(buf, host, n);
    buf[n] = '\0';
    return 1;
}

int hl_cap_kv_check_dsn(const struct HlManifestKvDynamic *policy, const char *dsn,
                        char *errbuf, size_t errlen)
{
    if (!dsn || !dsn[0]) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "kv.open: empty DSN");
        return -1;
    }
    if (!policy || !policy->declared) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen,
                     "kv.open requires a kv.dynamic policy in the manifest "
                     "(kv = { dynamic = { schemes = {...}, hosts = {...} } })");
        return -1;
    }

    char scheme[24];
    kv_dsn_scheme(dsn, scheme, sizeof scheme);
    if (!scheme[0]) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "kv.open: DSN needs a scheme (valkey:// / redis://)");
        return -1;
    }
    int scheme_ok = 0;
    for (int i = 0; i < policy->scheme_count; i++)
        if (strcmp(policy->schemes[i], scheme) == 0) { scheme_ok = 1; break; }
    if (!scheme_ok) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "kv.open: DSN scheme '%s' not in kv.dynamic.schemes", scheme);
        return -1;
    }

    char host[256];
    if (!kv_dsn_host(dsn, host, sizeof host)) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "kv.open: could not parse host from DSN");
        return -1;
    }
    if (!hl_host_match_any_env(policy->hosts, policy->host_count, host)) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "kv.open: host '%s' not allowed by kv.dynamic.hosts", host);
        return -1;
    }
    return 0;
}
