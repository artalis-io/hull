/*
 * test_net_resolve.c - the shared resolver (cap/net_resolve.c).
 *
 * Literals and refusals need no network. "localhost" goes through the system
 * resolver, which every supported host answers from its hosts file.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"

#include "hull/cap/net_resolve.h"

UTEST(net_resolve, an_ipv4_literal_needs_no_lookup)
{
    KlSockAddr a[4];
    ASSERT_EQ(hl_net_resolve("192.0.2.7", 2222, a, 4), 1);
    ASSERT_EQ((int)kl_sockaddr_family(&a[0]), (int)KL_AF_INET);
    ASSERT_EQ((int)kl_sockaddr_port(&a[0]), 2222);
}

UTEST(net_resolve, an_ipv6_literal_needs_no_lookup)
{
    KlSockAddr a[4];
    ASSERT_EQ(hl_net_resolve("2001:db8::1", 22, a, 4), 1);
    ASSERT_EQ((int)kl_sockaddr_family(&a[0]), (int)KL_AF_INET6);
    ASSERT_EQ((int)kl_sockaddr_port(&a[0]), 22);
}

UTEST(net_resolve, a_name_goes_through_the_system_resolver)
{
    KlSockAddr a[8];
    int n = hl_net_resolve("localhost", 5432, a, 8);
    ASSERT_GE(n, 1);
    for (int i = 0; i < n; i++)
        ASSERT_EQ((int)kl_sockaddr_port(&a[i]), 5432);
}

UTEST(net_resolve, never_writes_past_max)
{
    KlSockAddr a[2];
    ASSERT_LE(hl_net_resolve("localhost", 80, a, 1), 1);
}

UTEST(net_resolve, refusals_resolve_nothing)
{
    KlSockAddr a[2];
    ASSERT_EQ(hl_net_resolve(NULL, 22, a, 2), 0);
    ASSERT_EQ(hl_net_resolve("", 22, a, 2), 0);
    ASSERT_EQ(hl_net_resolve("192.0.2.7", 0, a, 2), 0);
    ASSERT_EQ(hl_net_resolve("192.0.2.7", 65536, a, 2), 0);
    ASSERT_EQ(hl_net_resolve("192.0.2.7", 22, NULL, 2), 0);
    ASSERT_EQ(hl_net_resolve("192.0.2.7", 22, a, 0), 0);
    ASSERT_EQ(hl_net_resolve("no-such-host.invalid", 22, a, 2), 0);
}

UTEST_MAIN()
