/**
 * @file cap/ssh_policy.h
 * @brief The SSH grants: which host, port and login; and which relay.
 *
 * Built on the generic reach check (cap/net_policy.h), adding the two things a
 * host/port grant cannot express: the login the app may authenticate as, and
 * the relay a tunnelled connection is dialled through. Like the reach check,
 * each function is a pure function of the manifest plus the destination, so a
 * denial happens before anything is observable from the network.
 *
 * Fails closed. Every path that is not an explicit allow is a deny:
 *
 *   - no `ssh` key in the manifest            -> HL_SSH_DENY_UNDECLARED
 *   - `ssh = {}` with no connect policy       -> HL_SSH_DENY_NO_POLICY
 *   - a connect policy with no hosts or ports -> HL_SSH_DENY_NO_POLICY
 *   - host not matching any pattern           -> HL_SSH_DENY_HOST
 *   - port not in the port list               -> HL_SSH_DENY_PORT
 *   - user not in the user list               -> HL_SSH_DENY_USER
 *   - a NULL grant                            -> HL_SSH_DENY_NO_POLICY
 *
 * The distinct denial reasons exist so the error surfaced to an app can say
 * which rule refused it. "You did not declare ssh" and "10.0.0.5 is not in your
 * allowlist" are different problems for whoever is reading the message, and
 * collapsing them wastes the one chance to explain the failure.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_SSH_POLICY_H
#define HULL_CAP_SSH_POLICY_H

#include "hull/manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an SSH authorization check. Only HL_SSH_ALLOW permits one. */
typedef enum HlSshAuth {
    HL_SSH_ALLOW = 0,
    HL_SSH_DENY_UNDECLARED,   /* the manifest has no `ssh` key at all */
    HL_SSH_DENY_NO_POLICY,    /* `ssh` present but no usable connect policy */
    HL_SSH_DENY_HOST,         /* host matched no pattern in ssh.connect.hosts */
    HL_SSH_DENY_PORT,         /* port is not in ssh.connect.ports */
    HL_SSH_DENY_USER,         /* login is not in ssh.connect.users */
    /* A tunnelled connect checks two grants, and "denied" is useless to
     * someone holding two allowlists: the message has to say which one
     * refused, and whether it refused the relay or the destination. */
    HL_SSH_DENY_TUNNEL_UNDECLARED, /* no `ssh.tunnel` key; no relay allowed */
    HL_SSH_DENY_TUNNEL_NO_POLICY,  /* `ssh.tunnel` present but grants nothing */
    HL_SSH_DENY_TUNNEL_HOST,       /* relay host is not in ssh.tunnel.hosts */
    HL_SSH_DENY_TUNNEL_PORT        /* relay port is not in ssh.tunnel.ports */
} HlSshAuth;

/**
 * Authorize one SSH connection: the reach check, plus the login.
 *
 * @param ssh   the manifest's `ssh` section (NULL denies)
 * @param host  hostname or IP literal (NULL or empty denies)
 * @param port  TCP port, 1..65535 (anything else denies)
 * @param user  the login to authenticate as (NULL or empty denies)
 * @return HL_SSH_ALLOW, or the specific reason for refusal
 *
 * The user is matched EXACTLY and case-sensitively. Unix logins are
 * case-sensitive, and the glob / CIDR vocabulary that makes sense for hosts
 * would only invite `*` here, which is the grant this check exists to refuse.
 */
HlSshAuth hl_ssh_check_connect(const HlManifestSsh *ssh, const char *host,
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
 * @return HL_SSH_ALLOW, or the specific reason for refusal
 *
 * Fails closed: no `ssh.tunnel` key, or an empty one, permits no tunnel.
 * There is no `users` here - the login belongs to the target, and the relay
 * never sees it.
 */
HlSshAuth hl_ssh_check_tunnel(const HlManifestSsh *ssh, const char *host,
                              int port);

/** Stable, human-readable reason for a denial. Never NULL. */
const char *hl_ssh_auth_reason(HlSshAuth a);

#ifdef __cplusplus
}
#endif

#endif /* HULL_CAP_SSH_POLICY_H */
