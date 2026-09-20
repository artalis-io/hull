/*
 * test_net_policy.c - the hull/net authorization gate.
 *
 * hull/net is the broadest network authority Hull grants, so the gate is tested
 * on its own, with a hand-built manifest and no socket, resolver or event loop
 * anywhere in scope. That is the point: the invariant is "denial happens BEFORE
 * network access", and a check that is a pure function of manifest plus
 * destination cannot race a DNS query that was never started.
 *
 * The deny cases matter more than the allow case here, and there are more of
 * them, because every one of them is a way an app could otherwise have reached
 * the network without saying so.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include <string.h>

#include "hull/cap/net.h"

/* Build a manifest granting exactly the given hosts and ports. */
static void mf_init(HlManifest *m)
{
    memset(m, 0, sizeof *m);
}

static void mf_allow(HlManifest *m, const char *host, int port)
{
    m->net.declared = 1;
    m->net.connect.declared = 1;
    if (host) {
        m->net.connect.hosts[m->net.connect.host_count++] = host;
    }
    if (port) {
        m->net.connect.ports[m->net.connect.port_count++] = port;
    }
}

/* ── the allow path ─────────────────────────────────────────────────── */

UTEST(net_policy, exact_host_and_port_allowed)
{
    HlManifest m; mf_init(&m);
    mf_allow(&m, "spark-7468.local", 22);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "spark-7468.local", 22), (int)HL_NET_ALLOW);
}

UTEST(net_policy, suffix_glob_matches_subdomain)
{
    HlManifest m; mf_init(&m);
    mf_allow(&m, "*.local", 22);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "spark-7468.local", 22), (int)HL_NET_ALLOW);
}

UTEST(net_policy, host_match_is_case_insensitive)
{
    HlManifest m; mf_init(&m);
    mf_allow(&m, "spark.local", 22);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "SPARK.LOCAL", 22), (int)HL_NET_ALLOW);
}

UTEST(net_policy, second_port_in_the_list_is_allowed)
{
    HlManifest m; mf_init(&m);
    mf_allow(&m, "h.local", 22);
    m.net.connect.ports[m.net.connect.port_count++] = 2222;
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "h.local", 2222), (int)HL_NET_ALLOW);
}

/* ── fail-closed: the three ways a policy can grant nothing ─────────── */

UTEST(net_policy, no_net_key_denies)
{
    HlManifest m; mf_init(&m);          /* nothing declared at all */
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "spark.local", 22),
              (int)HL_NET_DENY_UNDECLARED);
}

UTEST(net_policy, net_declared_without_connect_denies)
{
    HlManifest m; mf_init(&m);
    m.net.declared = 1;                 /* net = {} */
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "spark.local", 22),
              (int)HL_NET_DENY_NO_POLICY);
}

UTEST(net_policy, empty_host_list_is_not_a_wildcard)
{
    /* `net = { connect = { ports = {22} } }` reads like a grant and must not
     * behave like one. An absent host list is an empty grant. */
    HlManifest m; mf_init(&m);
    m.net.declared = 1;
    m.net.connect.declared = 1;
    m.net.connect.ports[m.net.connect.port_count++] = 22;
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "spark.local", 22),
              (int)HL_NET_DENY_NO_POLICY);
}

UTEST(net_policy, empty_port_list_is_not_a_wildcard)
{
    /* Ports are required and never inferred, even though 22 is the obvious
     * intent of an SSH-shaped policy. */
    HlManifest m; mf_init(&m);
    m.net.declared = 1;
    m.net.connect.declared = 1;
    m.net.connect.hosts[m.net.connect.host_count++] = "*.local";
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "spark.local", 22),
              (int)HL_NET_DENY_NO_POLICY);
}

/* ── fail-closed: destination outside the grant ─────────────────────── */

UTEST(net_policy, unlisted_host_denied)
{
    HlManifest m; mf_init(&m);
    mf_allow(&m, "spark.local", 22);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "evil.example.com", 22),
              (int)HL_NET_DENY_HOST);
}

UTEST(net_policy, unlisted_port_denied)
{
    HlManifest m; mf_init(&m);
    mf_allow(&m, "spark.local", 22);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "spark.local", 2222),
              (int)HL_NET_DENY_PORT);
}

UTEST(net_policy, suffix_glob_does_not_match_the_bare_suffix)
{
    /* "*.local" grants subdomains, not "local" itself. */
    HlManifest m; mf_init(&m);
    mf_allow(&m, "*.local", 22);
    ASSERT_NE((int)hl_cap_net_check_connect(&m, "local", 22), (int)HL_NET_ALLOW);
}

UTEST(net_policy, suffix_glob_does_not_match_a_longer_sibling_domain)
{
    /* The classic allowlist bug: "*.local" must not admit "notlocal" or
     * "evil.local.attacker.com". */
    HlManifest m; mf_init(&m);
    mf_allow(&m, "*.local", 22);
    ASSERT_NE((int)hl_cap_net_check_connect(&m, "notlocal", 22), (int)HL_NET_ALLOW);
    ASSERT_NE((int)hl_cap_net_check_connect(&m, "spark.local.attacker.com", 22),
              (int)HL_NET_ALLOW);
}

/* ── malformed input ────────────────────────────────────────────────── */

UTEST(net_policy, null_manifest_denies)
{
    /* Reaching here without a manifest is a programming error, and the safe
     * reading of a programming error is deny, not trust. */
    ASSERT_NE((int)hl_cap_net_check_connect(NULL, "spark.local", 22), (int)HL_NET_ALLOW);
}

UTEST(net_policy, null_or_empty_host_denies)
{
    HlManifest m; mf_init(&m);
    mf_allow(&m, "*", 22);              /* even under a wildcard */
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, NULL, 22), (int)HL_NET_DENY_HOST);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "", 22),   (int)HL_NET_DENY_HOST);
}

UTEST(net_policy, out_of_range_ports_denied)
{
    HlManifest m; mf_init(&m);
    mf_allow(&m, "*", 22);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "h", 0),      (int)HL_NET_DENY_PORT);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "h", -1),     (int)HL_NET_DENY_PORT);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "h", 65536),  (int)HL_NET_DENY_PORT);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "h", 100000), (int)HL_NET_DENY_PORT);
}

/* ── the wildcard is a real grant, and says so ──────────────────────── */

UTEST(net_policy, explicit_wildcard_host_allows_but_port_still_gates)
{
    /* "*" is a deliberate choice an app can make, and it still does not open
     * every port: the two lists are independent. */
    HlManifest m; mf_init(&m);
    mf_allow(&m, "*", 22);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "anything.example.com", 22),
              (int)HL_NET_ALLOW);
    ASSERT_EQ((int)hl_cap_net_check_connect(&m, "anything.example.com", 23),
              (int)HL_NET_DENY_PORT);
}

/* ── denial reasons are distinguishable ─────────────────────────────── */

UTEST(net_policy, every_denial_has_its_own_reason_string)
{
    /* The reasons exist so a message can say WHICH rule refused. If they
     * collapse to one string the distinction is decorative. */
    const char *r[5];
    r[0] = hl_cap_net_auth_reason(HL_NET_ALLOW);
    r[1] = hl_cap_net_auth_reason(HL_NET_DENY_UNDECLARED);
    r[2] = hl_cap_net_auth_reason(HL_NET_DENY_NO_POLICY);
    r[3] = hl_cap_net_auth_reason(HL_NET_DENY_HOST);
    r[4] = hl_cap_net_auth_reason(HL_NET_DENY_PORT);
    for (int i = 0; i < 5; i++) {
        ASSERT_NE(r[i], NULL);
        ASSERT_GT(strlen(r[i]), (size_t)0);
        for (int j = i + 1; j < 5; j++)
            ASSERT_NE(strcmp(r[i], r[j]), 0);
    }
}

UTEST_MAIN()
