/*
 * cap/net_policy.c - the hull/net authorization gate.
 *
 * Pure policy: manifest plus destination in, allow-or-reason out. No socket, no
 * resolver, no event loop. See include/hull/cap/net.h for why the check is
 * separated from the transport.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/net.h"
#include "hull/host_match.h"

HlNetAuth hl_cap_net_check_connect(const HlManifest *m, const char *host, int port)
{
    /* A missing manifest is not "no restrictions"; it is no grant. Treated the
     * same as an undeclared policy rather than trusted, because the only way to
     * reach here without one is a programming error, and the safe reading of a
     * programming error is deny. */
    if (!m) return HL_NET_DENY_NO_POLICY;

    if (!m->net.declared)                return HL_NET_DENY_UNDECLARED;
    if (!m->net.connect.declared)        return HL_NET_DENY_NO_POLICY;

    /* An empty list is an empty grant, not a wildcard. Both lists must be
     * non-empty for any connection to be possible: `net = { connect = {} }`
     * reads like "allow connect" and must not behave like it. */
    if (m->net.connect.host_count <= 0)  return HL_NET_DENY_NO_POLICY;
    if (m->net.connect.port_count <= 0)  return HL_NET_DENY_NO_POLICY;

    if (!host || !host[0])               return HL_NET_DENY_HOST;

    /* Port first: it is a bounded integer compare, while the host check walks
     * patterns and may resolve "$VAR" env refs. Cheapest decisive test first,
     * and it keeps a denied port from doing any environment lookup at all. */
    if (port < 1 || port > 65535)        return HL_NET_DENY_PORT;
    {
        int ok = 0;
        for (int i = 0; i < m->net.connect.port_count; i++) {
            if (m->net.connect.ports[i] == port) { ok = 1; break; }
        }
        if (!ok) return HL_NET_DENY_PORT;
    }

    /* Same matcher http / ws / smtp / databases.dynamic / kv.dynamic use:
     * exact (case-insensitive), "*", "*.suffix" subdomain glob, CIDR against IP
     * literals only, and "$VAR" / "${VAR}" env references resolved here. */
    if (!hl_host_match_any_env(m->net.connect.hosts,
                               m->net.connect.host_count, host))
        return HL_NET_DENY_HOST;

    return HL_NET_ALLOW;
}

const char *hl_cap_net_auth_reason(HlNetAuth a)
{
    switch (a) {
    case HL_NET_ALLOW:
        return "allowed";
    case HL_NET_DENY_UNDECLARED:
        return "no `net` capability in the manifest; "
               "add net = { connect = { hosts = {...}, ports = {...} } }";
    case HL_NET_DENY_NO_POLICY:
        return "`net` is declared but grants nothing; "
               "connect needs a non-empty hosts list and a non-empty ports list";
    case HL_NET_DENY_HOST:
        return "host is not in net.connect.hosts";
    case HL_NET_DENY_PORT:
        return "port is not in net.connect.ports";
    }
    return "denied";
}
