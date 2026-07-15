/*
 * host_match.c: host-allowlist pattern matching
 *
 * See host_match.h. Leaf util (exact/glob/CIDR matching). The one Hull
 * dependency is hl_manifest_env_ref (a header-only generic "$VAR" parser in
 * manifest.h) used by the *_env variants; relocating that parser to a neutral
 * util header would drop even this include (tracked as a §2.7 cleanup).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/host_match.h"
#include "hull/manifest.h"   /* hl_manifest_env_ref (header-only inline) */

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
        if (hl_manifest_env_ref(pat, var, sizeof var)) {
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
