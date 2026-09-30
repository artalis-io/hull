/*
 * test_ssh.c - hl_cap_ssh_open's refusals (cap/ssh.c).
 *
 * Every case here is refused before a name is resolved or a socket exists,
 * so no loop or pool is wired: a refusal that reached the transport would
 * fail with HL_NET_E_INVAL (no pool) instead of the answer asserted, and the
 * stream pointer would not stay NULL. The allow path is covered live by
 * tests/e2e_ssh_tunnel.sh.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"

#include <string.h>

#include "hull/cap/ssh.h"

static void grant(HlManifestSsh *g)
{
    memset(g, 0, sizeof *g);
    g->declared = 1;
    g->connect.declared = 1;
    g->connect.hosts[g->connect.host_count++] = "spark.local";
    g->connect.ports[g->connect.port_count++] = 22;
    g->users[g->user_count++] = "operator";
}

static HlSshOpen direct(void)
{
    HlSshOpen r;
    memset(&r, 0, sizeof r);
    r.host = "spark.local";
    r.port = 22;
    r.user = "operator";
    r.connect_ms = 1000;
    return r;
}

UTEST(cap_ssh, a_destination_outside_the_grant_is_denied_with_its_rule)
{
    HlManifestSsh g; grant(&g);
    HlSshEnv env = { .policy = &g };
    HlSshOpen r = direct();
    r.user = "root";
    HlNetStream *s = (HlNetStream *)1;
    const char *why = NULL;
    ASSERT_EQ(hl_cap_ssh_open(&env, &r, &s, &why), HL_NET_E_DENIED);
    ASSERT_TRUE(s == NULL);
    ASSERT_STREQ(why, "user is not in ssh.connect.users");
}

UTEST(cap_ssh, no_policy_denies)
{
    HlSshEnv env = { .policy = NULL };
    HlSshOpen r = direct();
    HlNetStream *s = NULL;
    const char *why = NULL;
    ASSERT_EQ(hl_cap_ssh_open(&env, &r, &s, &why), HL_NET_E_DENIED);
    ASSERT_TRUE(why != NULL);
}

UTEST(cap_ssh, a_relay_needs_its_own_grant)
{
    HlManifestSsh g; grant(&g);
    HlSshEnv env = { .policy = &g };
    HlSshOpen r = direct();
    r.via = 1;
    r.via_host = "relay.example.com";
    r.via_port = 443;
    HlNetStream *s = NULL;
    const char *why = NULL;
    ASSERT_EQ(hl_cap_ssh_open(&env, &r, &s, &why), HL_NET_E_DENIED);
    ASSERT_TRUE(strstr(why, "ssh.tunnel") != NULL);
}

UTEST(cap_ssh, a_relay_without_a_host_is_refused_not_dialled_directly)
{
    /* The destination is granted, so reading "no relay host" as "no relay"
     * would connect - directly, to a host the caller meant to reach only
     * through a relay whose headers carry its credentials. */
    HlManifestSsh g; grant(&g);
    g.tunnel.declared = 1;
    g.tunnel.hosts[g.tunnel.host_count++] = "*";
    g.tunnel.ports[g.tunnel.port_count++] = 443;
    HlSshEnv env = { .policy = &g };
    HlSshOpen r = direct();
    r.via = 1;
    r.via_port = 443;
    HlNetStream *s = NULL;
    const char *why = NULL;
    ASSERT_EQ(hl_cap_ssh_open(&env, &r, &s, &why), HL_NET_E_DENIED);
    ASSERT_STREQ(why, "tunnel host is not in ssh.tunnel.hosts");
}

UTEST(cap_ssh, a_tls_relay_without_a_trust_anchor_is_refused_not_downgraded)
{
    HlManifestSsh g; grant(&g);
    g.tunnel.declared = 1;
    g.tunnel.hosts[g.tunnel.host_count++] = "relay.example.com";
    g.tunnel.ports[g.tunnel.port_count++] = 443;
    HlClientTls no_anchor = { .cfg = NULL, .verifies = 0 };
    HlSshOpen r = direct();
    r.via = 1;
    r.via_host = "relay.example.com";
    r.via_port = 443;
    r.via_tls = 1;

    const HlClientTls *anchors[] = { NULL, &no_anchor };
    for (int i = 0; i < 2; i++) {
        HlSshEnv env = { .policy = &g, .client_tls = anchors[i] };
        HlNetStream *s = NULL;
        const char *why = NULL;
        ASSERT_EQ(hl_cap_ssh_open(&env, &r, &s, &why), HL_NET_E_TLS);
        ASSERT_TRUE(s == NULL);
        ASSERT_TRUE(why != NULL && strstr(why, "trust anchor") != NULL);
    }
}

UTEST(cap_ssh, the_grant_is_checked_before_the_trust_anchor)
{
    /* A TLS relay the manifest does not allow is a denial, not a TLS error:
     * the app learns which rule to change. */
    HlManifestSsh g; grant(&g);
    HlSshEnv env = { .policy = &g, .client_tls = NULL };
    HlSshOpen r = direct();
    r.via = 1;
    r.via_host = "relay.example.com";
    r.via_port = 443;
    r.via_tls = 1;
    HlNetStream *s = NULL;
    const char *why = NULL;
    ASSERT_EQ(hl_cap_ssh_open(&env, &r, &s, &why), HL_NET_E_DENIED);
}

UTEST(cap_ssh, bad_arguments_are_refused)
{
    HlManifestSsh g; grant(&g);
    HlSshEnv env = { .policy = &g };
    HlSshOpen r = direct();
    HlNetStream *s = NULL;
    ASSERT_EQ(hl_cap_ssh_open(NULL, &r, &s, NULL), HL_NET_E_INVAL);
    ASSERT_EQ(hl_cap_ssh_open(&env, NULL, &s, NULL), HL_NET_E_INVAL);
    ASSERT_EQ(hl_cap_ssh_open(&env, &r, NULL, NULL), HL_NET_E_INVAL);
}

UTEST_MAIN()
