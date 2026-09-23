/**
 * @file cap/net_policy.h
 * @brief Outbound reach authorization, and the SSH grant built on it.
 *
 * This header is the POLICY half of the private outbound stream. It answers
 * "may this app reach this host and port", with no socket, no resolver and no
 * event loop in scope. The transport half (cap/net_stream.h) calls it before
 * doing anything observable from the network.
 *
 * Two layers, deliberately: `hl_net_check_connect` is a GENERIC reach check
 * over a host/port grant and knows nothing about SSH, while
 * `hl_ssh_check_connect` adds the one thing a reach grant cannot express -
 * which login the app may authenticate as. Keeping the generic half generic is
 * what lets a second protocol reuse it without inheriting SSH's vocabulary.
 *
 * That split is deliberate. The security invariant Hull wants here is
 * "capability denial occurs BEFORE network access", and the cheapest way to
 * guarantee it is to make the check a pure function of the manifest plus the
 * requested destination, testable on its own. A denial cannot race a DNS query
 * that was never started.
 *
 * Fails closed. Every path that is not an explicit allow is a deny:
 *
 *   - no `ssh` key in the manifest            -> HL_NET_DENY_UNDECLARED
 *   - `ssh = {}` with no connect policy       -> HL_NET_DENY_NO_POLICY
 *   - a connect policy with no hosts or ports -> HL_NET_DENY_NO_POLICY
 *   - host not matching any pattern           -> HL_NET_DENY_HOST
 *   - port not in the port list               -> HL_NET_DENY_PORT
 *   - user not in the user list               -> HL_NET_DENY_USER
 *   - a NULL grant, host or user              -> HL_NET_DENY_NO_POLICY
 *
 * The distinct denial reasons exist so the error surfaced to an app can say
 * which rule refused it. "You did not declare ssh" and "10.0.0.5 is not in your
 * allowlist" are different problems for whoever is reading the message, and
 * collapsing them wastes the one chance to explain the failure.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_NET_POLICY_H
#define HULL_CAP_NET_POLICY_H

#include "hull/manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an authorization check. Only HL_NET_ALLOW permits a connection. */
typedef enum HlNetAuth {
    HL_NET_ALLOW = 0,
    HL_NET_DENY_UNDECLARED,   /* the manifest has no `ssh` key at all */
    HL_NET_DENY_NO_POLICY,    /* `ssh` present but no usable connect policy */
    HL_NET_DENY_HOST,         /* host matched no pattern in the allowlist */
    HL_NET_DENY_PORT,         /* port is not in the allowed port list */
    HL_NET_DENY_USER,         /* login is not in the allowed user list */
    /* Tunnel reasons are their OWN values, appended so the existing ones keep
     * their numbers. A tunnelled connect checks two grants, and "denied" is
     * useless to someone holding two allowlists: the message has to say which
     * one refused, and whether it refused the relay or the destination. */
    HL_NET_DENY_TUNNEL_UNDECLARED, /* no `ssh.tunnel` key; no relay allowed */
    HL_NET_DENY_TUNNEL_NO_POLICY,  /* `ssh.tunnel` present but grants nothing */
    HL_NET_DENY_TUNNEL_HOST,       /* relay host is not in ssh.tunnel.hosts */
    HL_NET_DENY_TUNNEL_PORT        /* relay port is not in ssh.tunnel.ports */
} HlNetAuth;

/**
 * Authorize one outbound reach. Generic: no protocol in scope.
 *
 * @param g     the host/port grant (NULL denies)
 * @param host  hostname or IP literal (NULL or empty denies)
 * @param port  TCP port, 1..65535 (anything else denies)
 * @return HL_NET_ALLOW, or the specific reason for refusal
 *
 * Pure: no allocation, no I/O, no globals. Safe to call before the sandbox is
 * applied and safe to call from a test with a hand-built grant.
 */
HlNetAuth hl_net_check_connect(const HlManifestNetConnect *g,
                               const char *host, int port);

/**
 * Authorize one SSH connection: the reach check above, plus the login.
 *
 * @param ssh   the manifest's `ssh` section (NULL denies)
 * @param host  hostname or IP literal (NULL or empty denies)
 * @param port  TCP port, 1..65535 (anything else denies)
 * @param user  the login to authenticate as (NULL or empty denies)
 * @return HL_NET_ALLOW, or the specific reason for refusal
 *
 * The user is matched EXACTLY and case-sensitively. Unix logins are
 * case-sensitive, and the glob / CIDR vocabulary that makes sense for hosts
 * would only invite `*` here, which is the grant this check exists to refuse.
 */
HlNetAuth hl_ssh_check_connect(const HlManifestSsh *ssh, const char *host,
                               int port, const char *user);

/**
 * Authorize the RELAY a tunnelled SSH connection is dialled through.
 *
 * A tunnel splits one destination in two: the TCP connection goes to the
 * relay, while the SSH session, the host key and the login all belong to the
 * target behind it. `hl_ssh_check_connect` still gates the target - a tunnel
 * must never widen which machine may be reached or as whom - and this gates
 * the machine actually dialled. A tunnelled connect passes BOTH or is refused.
 *
 * Separate grants because collapsing them would silently authorise SSH to
 * every host behind an allowed relay: the target travels inside the tunnel's
 * own headers and never appears in the socket address, so a single list could
 * not tell the two apart.
 *
 * @param ssh   the manifest's `ssh` section (NULL denies)
 * @param host  relay hostname or IP literal (NULL or empty denies)
 * @param port  relay TCP port, 1..65535 (anything else denies)
 * @return HL_NET_ALLOW, or the specific reason for refusal
 *
 * Fails closed: no `ssh.tunnel` key, or an empty one, permits no tunnel.
 * There is no `users` here - the login belongs to the target, and the relay
 * never sees it.
 */
HlNetAuth hl_ssh_check_tunnel(const HlManifestSsh *ssh, const char *host,
                              int port);

/** Stable, human-readable reason for a denial. Never NULL. */
const char *hl_cap_net_auth_reason(HlNetAuth a);

#ifdef __cplusplus
}
#endif

#endif /* HULL_CAP_NET_POLICY_H */
