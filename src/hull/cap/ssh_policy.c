/*
 * cap/ssh_policy.c - the SSH grants, on top of the generic reach gate.
 *
 * Pure policy, like cap/net_policy.c: manifest plus destination in,
 * allow-or-reason out. See include/hull/cap/ssh_policy.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "hull/cap/ssh_policy.h"
#include "hull/cap/net_policy.h"
#include "hull/utils/env_ref.h"   /* hl_env_ref: the "$VAR" parser hosts use */

/* The generic reasons, in the words of the grant that was checked. The two
 * tables differ only in which list they name, and they have to: a denial that
 * names ssh.connect.hosts when the RELAY was refused sends the reader to the
 * wrong list. */
static HlSshAuth from_reach(HlNetReach r, int tunnel)
{
    switch (r) {
    case HL_NET_REACH_OK:
        return HL_SSH_ALLOW;
    case HL_NET_REACH_NO_GRANT:
        return tunnel ? HL_SSH_DENY_TUNNEL_NO_POLICY : HL_SSH_DENY_NO_POLICY;
    case HL_NET_REACH_HOST:
        return tunnel ? HL_SSH_DENY_TUNNEL_HOST : HL_SSH_DENY_HOST;
    case HL_NET_REACH_PORT:
        return tunnel ? HL_SSH_DENY_TUNNEL_PORT : HL_SSH_DENY_PORT;
    }
    return tunnel ? HL_SSH_DENY_TUNNEL_NO_POLICY : HL_SSH_DENY_NO_POLICY;
}

/* The login a "$VAR" users entry resolves to, or NULL when the variable is
 * unset or its value is not a plausible login: empty, over 255 bytes, or
 * holding whitespace, a comma, a glob character (`*` `?` `[`), `/`, `$` or a
 * control byte. The environment is operator-controlled, not app-controlled,
 * but it is also where a review never looked, so what comes out of it is held
 * to the shape of one Unix account name and nothing wider. */
static const char *user_from_env(const char *var)
{
    const char *v = getenv(var);
    if (!v || !v[0]) return NULL;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++, n++) {
        if (n >= 255) return NULL;
        if (*p <= 0x20 || *p == 0x7f) return NULL;  /* control, space */
        switch (*p) {
        case ',': case '*': case '?': case '[': case '/': case '$':
            return NULL;
        default:
            break;
        }
    }
    return v;
}

HlSshAuth hl_ssh_check_connect(const HlManifestSsh *ssh, const char *host,
                               int port, const char *user)
{
    if (!ssh)           return HL_SSH_DENY_NO_POLICY;
    if (!ssh->declared) return HL_SSH_DENY_UNDECLARED;

    /* Reach first. A destination the app may not talk to at all is refused
     * before the login is even considered, so a denial names the broader
     * problem rather than the narrower one. */
    HlSshAuth a = from_reach(hl_net_check_reach(&ssh->connect, host, port), 0);
    if (a != HL_SSH_ALLOW) return a;

    if (ssh->user_count <= 0) return HL_SSH_DENY_NO_POLICY;
    if (!user || !user[0])    return HL_SSH_DENY_USER;

    /* Exact and case-sensitive. Unix logins are case-sensitive, and no glob
     * vocabulary is offered here on purpose: the only pattern anyone would
     * reach for is `*`, which is precisely the grant this check exists to
     * refuse. Listing three users costs three lines. The one exception is an
     * entry that is exactly "$VAR" / "${VAR}" (the same env reference hosts
     * take): it is read from the environment at check time and still names
     * ONE login - a value that is not a plausible login (user_from_env)
     * matches nothing, so the environment cannot widen the grant to `*`. */
    for (int i = 0; i < ssh->user_count; i++) {
        const char *want = ssh->users[i];
        if (!want) continue;
        char var[128];
        if (hl_env_ref(want, var, sizeof var)) {
            want = user_from_env(var);
            if (!want) continue;
        }
        if (strcmp(want, user) == 0)
            return HL_SSH_ALLOW;
    }
    return HL_SSH_DENY_USER;
}

HlSshAuth hl_ssh_check_tunnel(const HlManifestSsh *ssh, const char *host,
                              int port)
{
    if (!ssh)           return HL_SSH_DENY_NO_POLICY;
    if (!ssh->declared) return HL_SSH_DENY_UNDECLARED;

    /* No tunnel key means no tunnel, not "any tunnel". An app that reaches a
     * host directly today and gains a relay tomorrow has to say so, because
     * the relay terminates the TLS the app is trusting. */
    if (!ssh->tunnel.declared) return HL_SSH_DENY_TUNNEL_UNDECLARED;

    /* The same reach check as connect: ONE matcher and one fail-closed rule
     * for both grants; only the words differ. */
    return from_reach(hl_net_check_reach(&ssh->tunnel, host, port), 1);
}

const char *hl_ssh_auth_reason(HlSshAuth a)
{
    switch (a) {
    case HL_SSH_ALLOW:
        return "allowed";
    case HL_SSH_DENY_UNDECLARED:
        return "no `ssh` capability in the manifest; add "
               "ssh = { connect = { hosts = {...}, ports = {...}, users = {...} } }";
    case HL_SSH_DENY_NO_POLICY:
        return "`ssh` is declared but grants nothing; connect needs a non-empty "
               "hosts list, a non-empty ports list and a non-empty users list";
    case HL_SSH_DENY_HOST:
        return "host is not in ssh.connect.hosts";
    case HL_SSH_DENY_PORT:
        return "port is not in ssh.connect.ports";
    case HL_SSH_DENY_USER:
        return "user is not in ssh.connect.users";
    case HL_SSH_DENY_TUNNEL_UNDECLARED:
        return "no `ssh.tunnel` in the manifest; reaching a host through a "
               "relay needs ssh = { tunnel = { hosts = {...}, ports = {...} } } "
               "as well as ssh.connect for the host behind it";
    case HL_SSH_DENY_TUNNEL_NO_POLICY:
        return "`ssh.tunnel` is declared but grants nothing; it needs a "
               "non-empty hosts list and a non-empty ports list";
    case HL_SSH_DENY_TUNNEL_HOST:
        return "tunnel host is not in ssh.tunnel.hosts";
    case HL_SSH_DENY_TUNNEL_PORT:
        return "tunnel port is not in ssh.tunnel.ports";
    }
    return "denied";
}
