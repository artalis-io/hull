/*
 * cap/net_policy.c - the outbound reach gate, and the SSH grant on top of it.
 *
 * Pure policy: grant plus destination in, allow-or-reason out. No socket, no
 * resolver, no event loop. See include/hull/cap/net_policy.h for why the check
 * is separated from the transport.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>

#include "hull/cap/net_policy.h"
#include "hull/host_match.h"

HlNetAuth hl_net_check_connect(const HlManifestNetConnect *g,
                               const char *host, int port)
{
    /* A missing grant is not "no restrictions"; it is no grant. Treated the
     * same as an undeclared policy rather than trusted, because the only way to
     * reach here without one is a programming error, and the safe reading of a
     * programming error is deny. */
    if (!g)                  return HL_NET_DENY_NO_POLICY;
    if (!g->declared)        return HL_NET_DENY_NO_POLICY;

    /* An empty list is an empty grant, not a wildcard. Both lists must be
     * non-empty for any connection to be possible: `connect = {}` reads like
     * "allow connect" and must not behave like it. */
    if (g->host_count <= 0)  return HL_NET_DENY_NO_POLICY;
    if (g->port_count <= 0)  return HL_NET_DENY_NO_POLICY;

    if (!host || !host[0])   return HL_NET_DENY_HOST;

    /* Port first: it is a bounded integer compare, while the host check walks
     * patterns and may resolve "$VAR" env refs. Cheapest decisive test first,
     * and it keeps a denied port from doing any environment lookup at all. */
    if (port < 1 || port > 65535) return HL_NET_DENY_PORT;
    {
        int ok = 0;
        for (int i = 0; i < g->port_count; i++) {
            if (g->ports[i] == port) { ok = 1; break; }
        }
        if (!ok) return HL_NET_DENY_PORT;
    }

    /* Same matcher http / ws / smtp / databases.dynamic / kv.dynamic use:
     * exact (case-insensitive), "*", "*.suffix" subdomain glob, CIDR against IP
     * literals only, and "$VAR" / "${VAR}" env references resolved here. */
    if (!hl_host_match_any_env(g->hosts, g->host_count, host))
        return HL_NET_DENY_HOST;

    return HL_NET_ALLOW;
}

HlNetAuth hl_ssh_check_connect(const HlManifestSsh *ssh, const char *host,
                               int port, const char *user)
{
    if (!ssh)           return HL_NET_DENY_NO_POLICY;
    if (!ssh->declared) return HL_NET_DENY_UNDECLARED;

    /* Reach first. A destination the app may not talk to at all is refused
     * before the login is even considered, so a denial names the broader
     * problem rather than the narrower one. */
    HlNetAuth a = hl_net_check_connect(&ssh->connect, host, port);
    if (a != HL_NET_ALLOW) return a;

    if (ssh->user_count <= 0) return HL_NET_DENY_NO_POLICY;
    if (!user || !user[0])    return HL_NET_DENY_USER;

    /* Exact and case-sensitive. Unix logins are case-sensitive, and no glob
     * vocabulary is offered here on purpose: the only pattern anyone would
     * reach for is `*`, which is precisely the grant this check exists to
     * refuse. Listing three users costs three lines. */
    for (int i = 0; i < ssh->user_count; i++) {
        if (ssh->users[i] && strcmp(ssh->users[i], user) == 0)
            return HL_NET_ALLOW;
    }
    return HL_NET_DENY_USER;
}

const char *hl_cap_net_auth_reason(HlNetAuth a)
{
    switch (a) {
    case HL_NET_ALLOW:
        return "allowed";
    case HL_NET_DENY_UNDECLARED:
        return "no `ssh` capability in the manifest; add "
               "ssh = { connect = { hosts = {...}, ports = {...}, users = {...} } }";
    case HL_NET_DENY_NO_POLICY:
        return "`ssh` is declared but grants nothing; connect needs a non-empty "
               "hosts list, a non-empty ports list and a non-empty users list";
    case HL_NET_DENY_HOST:
        return "host is not in ssh.connect.hosts";
    case HL_NET_DENY_PORT:
        return "port is not in ssh.connect.ports";
    case HL_NET_DENY_USER:
        return "user is not in ssh.connect.users";
    }
    return "denied";
}
