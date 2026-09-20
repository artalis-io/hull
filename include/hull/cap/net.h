/**
 * @file cap/net.h
 * @brief Outbound byte-stream capability: the authorization gate.
 *
 * This header is the POLICY half of hull/net. It answers exactly one question,
 * "may this app open a TCP connection to this host and port", and it answers it
 * with no socket, no resolver and no event loop in scope. The transport half
 * (connect / read / write / close / cancel) lands separately and calls this
 * before doing anything observable from the network.
 *
 * That split is deliberate. The security invariant Hull wants here is
 * "capability denial occurs BEFORE network access", and the cheapest way to
 * guarantee it is to make the check a pure function of the manifest plus the
 * requested destination, testable on its own. A denial cannot race a DNS query
 * that was never started.
 *
 * Fails closed. Every path that is not an explicit allow is a deny:
 *
 *   - no `net` key in the manifest            -> HL_NET_DENY_UNDECLARED
 *   - `net = {}` with no connect policy       -> HL_NET_DENY_NO_POLICY
 *   - a connect policy with no hosts or ports -> HL_NET_DENY_NO_POLICY
 *   - host not matching any pattern           -> HL_NET_DENY_HOST
 *   - port not in the port list               -> HL_NET_DENY_PORT
 *   - a NULL manifest or host                 -> HL_NET_DENY_NO_POLICY
 *
 * The distinct denial reasons exist so the error surfaced to an app can say
 * which rule refused it. "You did not declare net" and "10.0.0.5 is not in your
 * allowlist" are different problems for whoever is reading the message, and
 * collapsing them wastes the one chance to explain the failure.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_NET_H
#define HULL_CAP_NET_H

#include "hull/manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an authorization check. Only HL_NET_ALLOW permits a connection. */
typedef enum HlNetAuth {
    HL_NET_ALLOW = 0,
    HL_NET_DENY_UNDECLARED,   /* the manifest has no `net` key at all */
    HL_NET_DENY_NO_POLICY,    /* `net` present but no usable connect policy */
    HL_NET_DENY_HOST,         /* host matched no pattern in the allowlist */
    HL_NET_DENY_PORT          /* port is not in the allowed port list */
} HlNetAuth;

/**
 * Authorize one outbound connection.
 *
 * @param m     the app manifest (NULL denies)
 * @param host  hostname or IP literal (NULL or empty denies)
 * @param port  TCP port, 1..65535 (anything else denies)
 * @return HL_NET_ALLOW, or the specific reason for refusal
 *
 * Pure: no allocation, no I/O, no globals. Safe to call before the sandbox is
 * applied and safe to call from a test with a hand-built manifest.
 */
HlNetAuth hl_cap_net_check_connect(const HlManifest *m, const char *host, int port);

/** Stable, human-readable reason for a denial. Never NULL. */
const char *hl_cap_net_auth_reason(HlNetAuth a);

#ifdef __cplusplus
}
#endif

#endif /* HULL_CAP_NET_H */
