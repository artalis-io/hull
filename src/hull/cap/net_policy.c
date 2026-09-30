/*
 * cap/net_policy.c - the generic outbound reach gate.
 *
 * Pure policy: grant plus destination in, allow-or-reason out. No socket, no
 * resolver, no event loop, no protocol. See include/hull/cap/net_policy.h for
 * why the check is separated from the transport, and cap/ssh_policy.c for the
 * SSH grants built on it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/net_policy.h"
#include "hull/host_match.h"

HlNetReach hl_net_check_reach(const HlManifestNetConnect *g,
                              const char *host, int port)
{
    /* A missing grant is not "no restrictions"; it is no grant. Treated the
     * same as an undeclared policy rather than trusted, because the only way to
     * reach here without one is a programming error, and the safe reading of a
     * programming error is deny. */
    if (!g)                  return HL_NET_REACH_NO_GRANT;
    if (!g->declared)        return HL_NET_REACH_NO_GRANT;

    /* An empty list is an empty grant, not a wildcard. Both lists must be
     * non-empty for any connection to be possible: `connect = {}` reads like
     * "allow connect" and must not behave like it. */
    if (g->host_count <= 0)  return HL_NET_REACH_NO_GRANT;
    if (g->port_count <= 0)  return HL_NET_REACH_NO_GRANT;

    if (!host || !host[0])   return HL_NET_REACH_HOST;

    /* Port first: it is a bounded integer compare, while the host check walks
     * patterns and may resolve "$VAR" env refs. Cheapest decisive test first,
     * and it keeps a denied port from doing any environment lookup at all. */
    if (port < 1 || port > 65535) return HL_NET_REACH_PORT;
    {
        int ok = 0;
        for (int i = 0; i < g->port_count; i++) {
            if (g->ports[i] == port) { ok = 1; break; }
        }
        if (!ok) return HL_NET_REACH_PORT;
    }

    /* Same matcher http / ws / smtp / databases.dynamic / kv.dynamic use:
     * exact (case-insensitive), "*", "*.suffix" subdomain glob, CIDR against IP
     * literals only, and "$VAR" / "${VAR}" env references resolved here. */
    if (!hl_host_match_any_env(g->hosts, g->host_count, host))
        return HL_NET_REACH_HOST;

    return HL_NET_REACH_OK;
}
