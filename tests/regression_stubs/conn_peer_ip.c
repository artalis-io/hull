/*
 * Pre-fix stand-in for hl_conn_peer_ip (src/hull/cap/conn_limit.c), linked
 * only by scripts/check_regression_test.sh (docs/design/audit_convergence.md
 * 5.1). Before the fix the request limiter matched the raw peer string
 * against limits.trusted_proxies, so an IPv4-mapped peer stayed mapped.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/cap/conn_limit.h"

const char *hl_conn_peer_ip(const char *ip, char *buf, size_t n)
{
    (void)buf; (void)n;
    return ip;
}
