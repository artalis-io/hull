/*
 * host_match.c: host-allowlist pattern matching
 *
 * See host_match.h. Self-contained leaf util (exact/glob/CIDR matching + the
 * "$VAR" env-ref resolution used by the *_env variants). No Hull-domain
 * dependency: hl_env_ref is a neutral header-only parser.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/host_match.h"
#include "hull/utils/env_ref.h"   /* hl_env_ref (header-only inline) */

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Canonical IP literals (audit 42) ─────────────────────────────────
 *
 * The matcher reads an IP literal with inet_pton; the connection later hands
 * the same string to getaddrinfo (or Keel's kl_sockaddr_parse). Those parsers
 * do not agree on every spelling: Cosmopolitan's inet_pton reads a leading-zero
 * octet as decimal and its getaddrinfo as octal, so "0177.0.0.1" was checked as
 * 177.0.0.1 and dialled 127.0.0.1, and inet_aton-style resolvers also take hex
 * ("0x7f.0.0.1"), shorthand ("127.1") and a single integer ("2130706433"). So
 * the matcher reads only spellings every parser reads the same way:
 *
 *   IPv4: exactly four dot-separated decimal octets, each 0-255, at most three
 *         digits, no leading zero except a lone "0";
 *   IPv6: hex groups of 1-4 digits, at most one "::", eight groups in all (or
 *         fewer with "::"), an optional embedded IPv4 tail in the IPv4 form
 *         above, and an optional zone ("%eth0") that is never part of a CIDR
 *         match.
 *
 * A host that LOOKS like an IP literal (contains ':', or every dot-separated
 * label is a number - decimal digits or 0x-hex) but is not in that form matches
 * no pattern at all, not even "*" or an exact entry spelled the same way: a
 * resolver may read it as an address the check never saw. */

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* 1 if [s, s+n) is a canonical dotted-quad. */
static int canon_v4(const char *s, size_t n)
{
    int octets = 0;
    size_t i = 0;
    while (octets < 4) {
        size_t start = i;
        unsigned v = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (i - start >= 3) return 0;
            v = v * 10 + (unsigned)(s[i] - '0');
            i++;
        }
        size_t len = i - start;
        if (len == 0 || v > 255) return 0;
        if (len > 1 && s[start] == '0') return 0;   /* leading zero */
        octets++;
        if (octets < 4) {
            if (i >= n || s[i] != '.') return 0;
            i++;
        }
    }
    return i == n;
}

/* 1 if [s, s+n) is a canonical IPv6 address (no zone). */
static int canon_v6(const char *s, size_t n)
{
    if (n < 2 || n > 45) return 0;
    int groups = 0, dcolon = 0;
    size_t i = 0;
    if (s[0] == ':') {
        if (s[1] != ':') return 0;
        dcolon = 1;
        i = 2;
        if (i == n) return 1;   /* "::" */
    }
    for (;;) {
        /* A field: a hex group, or (last) an IPv4 tail. */
        size_t start = i;
        while (i < n && is_hex(s[i])) i++;
        if (i < n && s[i] == '.') {
            if (!canon_v4(s + start, n - start)) return 0;
            groups += 2;
            break;
        }
        size_t len = i - start;
        if (len == 0 || len > 4) return 0;
        groups++;
        if (i == n) break;
        if (s[i] != ':') return 0;
        i++;
        if (i < n && s[i] == ':') {
            if (dcolon) return 0;
            dcolon = 1;
            i++;
            if (i == n) break;   /* trailing "::" */
        } else if (i == n) {
            return 0;            /* trailing single ':' */
        }
    }
    return dcolon ? groups <= 7 : groups == 8;
}

/* 1 if @p s (n bytes) is a number a resolver may read as an address part:
 * decimal digits, or "0x"/"0X" followed by hex digits. */
static int numeric_label(const char *s, size_t n)
{
    if (n == 0) return 0;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (size_t i = 2; i < n; i++) if (!is_hex(s[i])) return 0;
        return 1;
    }
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* 1 if @p host could be read as an IP literal by some resolver. */
static int ip_like(const char *host)
{
    if (strchr(host, ':')) return 1;
    size_t n = strlen(host);
    if (n && host[n - 1] == '.') n--;   /* a trailing root dot */
    if (n == 0) return 0;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || host[i] == '.') {
            if (!numeric_label(host + start, i - start)) return 0;
            start = i + 1;
        }
    }
    return 1;
}

int hl_host_ip_canonical(const char *host)
{
    if (!host || !*host) return 0;
    if (!strchr(host, ':')) return canon_v4(host, strlen(host));
    const char *pct = strchr(host, '%');
    size_t alen = pct ? (size_t)(pct - host) : strlen(host);
    if (pct) {
        /* Zone: unreserved characters (a URL's "%25" encoding included). */
        if (!pct[1]) return 0;
        for (const char *q = pct + 1; *q; q++) {
            char c = *q;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
                  c == '_' || c == '~' || c == '%'))
                return 0;
        }
    }
    return canon_v6(host, alen);
}

int hl_host_literal_ok(const char *host)
{
    if (!host || !*host) return 0;
    if (!ip_like(host)) return 1;          /* a DNS name */
    return hl_host_ip_canonical(host);
}

/* CIDR match: @p pattern is "net/prefix". Matches only when @p host is an IP
 * literal in the same family and inside the range. Returns 0 for a hostname
 * host (it is not an IP literal) or a malformed pattern. */
static int cidr_match(const char *pattern, const char *host)
{
    const char *slash = strchr(pattern, '/');
    if (!slash) return 0;

    char net[64];
    size_t nl = (size_t)(slash - pattern);
    if (nl == 0 || nl >= sizeof net) return 0;
    memcpy(net, pattern, nl);
    net[nl] = '\0';

    char *end = NULL;
    long prefix = strtol(slash + 1, &end, 10);
    if (end == slash + 1 || *end != '\0' || prefix < 0) return 0;

    unsigned char netb[16], hostb[16];
    int family, nbytes;
    /* Both sides canonical (audit 42); neither carries a zone. */
    if (!hl_host_ip_canonical(net) || strchr(net, '%') ||
        !hl_host_ip_canonical(host) || strchr(host, '%'))
        return 0;
    if (inet_pton(AF_INET, net, netb) == 1) {
        family = AF_INET; nbytes = 4;
        if (prefix > 32) return 0;
    } else if (inet_pton(AF_INET6, net, netb) == 1) {
        family = AF_INET6; nbytes = 16;
        if (prefix > 128) return 0;
    } else {
        return 0;
    }

    if (inet_pton(family, host, hostb) != 1)
        return 0;   /* host is not an IP literal of this family */

    int full = (int)prefix / 8, rem = (int)prefix % 8;
    if (full > nbytes) return 0;   /* defensive; prefix already bounded */
    if (full > 0 && memcmp(netb, hostb, (size_t)full) != 0)
        return 0;
    if (rem) {
        unsigned char mask = (unsigned char)(0xFF << (8 - rem));
        if ((netb[full] & mask) != (hostb[full] & mask))
            return 0;
    }
    return 1;
}

int hl_host_match(const char *pattern, const char *host)
{
    if (!pattern || !host || !*host) return 0;

    /* A host that looks like an IP literal but is not in the canonical form
     * matches nothing (audit 42): the dial may read it as another address. */
    if (!hl_host_literal_ok(host)) return 0;

    /* Any host. */
    if (pattern[0] == '*' && pattern[1] == '\0')
        return 1;

    /* CIDR (contains '/'). */
    if (strchr(pattern, '/'))
        return cidr_match(pattern, host);

    /* Subdomain glob "*.suffix": host must end with ".suffix" and have at least
     * one label before it (so "*.example.com" does not match "example.com"). */
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *suffix = pattern + 1;   /* ".example.com" */
        size_t sl = strlen(suffix), hl = strlen(host);
        if (hl <= sl) return 0;
        return strcasecmp(host + (hl - sl), suffix) == 0;
    }

    /* Exact, case-insensitive. */
    return strcasecmp(pattern, host) == 0;
}

int hl_host_match_any(const char *const *patterns, int n, const char *host)
{
    if (!patterns) return 0;
    for (int i = 0; i < n; i++)
        if (hl_host_match(patterns[i], host))
            return 1;
    return 0;
}

int hl_host_match_any_env(const char *const *patterns, int n, const char *host)
{
    if (!patterns || !host) return 0;
    char var[128];
    for (int i = 0; i < n; i++) {
        const char *pat = patterns[i];
        /* "$VAR" / "${VAR}" resolves from the environment; anything else
         * (including a literal that merely contains '$') is matched as-is. */
        if (hl_env_ref(pat, var, sizeof var)) {
            const char *v = getenv(var);
            if (v && v[0] && hl_host_match(v, host)) return 1;
        } else if (hl_host_match(pat, host)) {
            return 1;
        }
    }
    return 0;
}

int hl_host_match_any_env_n(const char *const *patterns, int n,
                            const char *host, size_t host_len)
{
    if (!host) return 0;
    char buf[256];
    if (host_len >= sizeof buf) return 0;   /* implausible host; fail closed */
    memcpy(buf, host, host_len);
    buf[host_len] = '\0';
    return hl_host_match_any_env(patterns, n, buf);
}

int hl_dsn_host(const char *dsn, char *buf, size_t bufsz)
{
    if (!dsn || !buf || bufsz == 0) return 0;
    const char *sep = strstr(dsn, "://");
    if (!sep) return 0;
    if (strchr(dsn, '#')) return 0;

    const char *p = sep + 3;
    /* Up to the path: a parser that ends the authority only at '/' still
     * sees every '@' counted here. */
    const char *pathstart = p + strcspn(p, "/");
    int ats = 0;
    for (const char *q = p; q < pathstart; q++) {
        unsigned char c = (unsigned char)*q;
        if (c < 0x20 || c == 0x7f) return 0;
        if (c == '@') ats++;
    }
    if (ats > 1) return 0;

    const char *end = p + strcspn(p, "/?");
    const char *at  = memchr(p, '@', (size_t)(end - p));
    const char *host = at ? at + 1 : p;

    const char *hend, *rest;
    if (host < end && *host == '[') {                 /* IPv6 literal */
        const char *close = memchr(host, ']', (size_t)(end - host));
        if (!close) return 0;
        for (const char *q = host + 1; q < close; q++) {
            char c = *q;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F') || c == ':' || c == '.'))
                return 0;
        }
        rest = close + 1;
        host++;
        hend = close;
    } else {
        hend = host;
        while (hend < end && *hend != ':') {
            char c = *hend;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_'))
                return 0;
            hend++;
        }
        rest = hend;
    }
    if (rest < end) {                                 /* ":port", digits */
        if (*rest != ':' || rest + 1 == end) return 0;
        for (const char *q = rest + 1; q < end; q++)
            if (*q < '0' || *q > '9') return 0;
    }

    size_t n = (size_t)(hend - host);
    if (n == 0 || n >= bufsz) return 0;
    memcpy(buf, host, n);
    buf[n] = '\0';
    return 1;
}
