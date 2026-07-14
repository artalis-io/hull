/*
 * fuzz_host_match.c: libFuzzer harness for the host-allowlist matcher.
 *
 * hl_host_match parses attacker-influenced allowlist patterns (glob/CIDR) and
 * hosts. This exercises the CIDR parse (inet_pton / strtol / bit compare), the
 * subdomain-glob suffix math, and the exact path under ASan+UBSan on arbitrary
 * input. Splits the fuzz buffer into "pattern \0 host" (or halves it when there
 * is no NUL).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/host_match.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const uint8_t *nul = size ? memchr(data, 0, size) : NULL;
    size_t plen   = nul ? (size_t)(nul - data) : size / 2;
    size_t hstart = nul ? plen + 1 : plen;
    size_t hlen   = size > hstart ? size - hstart : 0;

    char *pat  = malloc(plen + 1);
    char *host = malloc(hlen + 1);
    if (!pat || !host) { free(pat); free(host); return 0; }

    memcpy(pat, data, plen);         pat[plen]   = '\0';
    memcpy(host, data + hstart, hlen); host[hlen] = '\0';

    (void)hl_host_match(pat, host);
    const char *pats[] = { pat };
    (void)hl_host_match_any(pats, 1, host);

    free(pat);
    free(host);
    return 0;
}
