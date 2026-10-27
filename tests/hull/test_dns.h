/**
 * @file test_dns.h
 * @brief TEST-ONLY: detect a resolver that answers names that do not exist.
 *
 * Tests that expect a lookup to fail use a reserved name (`.invalid`, RFC
 * 2606 / RFC 6761, which a resolver must never answer). Some networks answer
 * it anyway: a captive portal, an ISP "search" redirect, or a router that
 * hands back its own address for every name (measured on one: every name,
 * `.invalid` included, resolved to 192.168.8.251). On such a network the
 * failure the test expects cannot happen, so the test skips instead of
 * failing on the network rather than on Hull.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_TEST_DNS_H
#define HL_TEST_DNS_H

#include <netdb.h>
#include <stddef.h>
#include <sys/socket.h>

/* 1 when the system resolver answers a name under `.invalid`. */
static inline int hl_test_resolver_answers_invalid(void)
{
    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("hull-resolver-probe.invalid", NULL, &hints, &res) != 0)
        return 0;
    freeaddrinfo(res);
    return 1;
}

#endif /* HL_TEST_DNS_H */
