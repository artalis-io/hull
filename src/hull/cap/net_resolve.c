/*
 * cap/net_resolve.c - the one getaddrinfo loop (see cap/net_resolve.h).
 *
 * SEAM: when Keel exports a system-resolver kl_resolve_sync (keel#332), this
 * body collapses to that call, and <netdb.h> and the sockaddr_in / in6 casts
 * leave Hull with it. Every caller is already written against this shape.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/net_resolve.h"

#include <netdb.h>
#include <sys/socket.h>   /* AF_UNSPEC / AF_INET / AF_INET6 (cosmo needs it explicit) */
#include <netinet/in.h>   /* struct sockaddr_in / sockaddr_in6 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int hl_net_resolve(const char *host, int port, KlSockAddr *out, int max)
{
    if (!host || !host[0] || !out || max < 1 || port < 1 || port > 65535)
        return 0;

    /* An address literal needs no resolver. kl_sockaddr_parse is numeric-only
     * (no DNS), so this cannot widen what a name resolves to; a literal with
     * a zone ("fe80::1%eth0") is not numeric to it and falls through to
     * getaddrinfo, which knows what an interface name means. */
    if (kl_sockaddr_parse(&out[0], host, (uint16_t)port) == 0)
        return 1;

    char port_str[8];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return 0;

    int n = 0;
    for (struct addrinfo *ai = res; ai && n < max; ai = ai->ai_next) {
        if (!ai->ai_addr) continue;
        if (ai->ai_family == AF_INET) {
            const struct sockaddr_in *v4 =
                (const struct sockaddr_in *)(const void *)ai->ai_addr;
            uint8_t ip[4];
            memcpy(ip, &v4->sin_addr, 4);
            if (kl_sockaddr_from_ipv4(&out[n], ip, (uint16_t)port) == 0) n++;
        } else if (ai->ai_family == AF_INET6) {
            const struct sockaddr_in6 *v6 =
                (const struct sockaddr_in6 *)(const void *)ai->ai_addr;
            uint8_t ip[16];
            memcpy(ip, &v6->sin6_addr, 16);
            /* Keep the scope: without it a link-local address names no
             * interface and the connect fails. */
            if (kl_sockaddr_from_ipv6(&out[n], ip, (uint16_t)port,
                                      v6->sin6_scope_id) == 0) n++;
        }
    }
    freeaddrinfo(res);
    return n;
}
