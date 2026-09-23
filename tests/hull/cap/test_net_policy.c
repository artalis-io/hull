/*
 * test_net_policy.c - the SSH connect authorization gate.
 *
 * This is the only key that grants outbound stream authority, so the gate is
 * tested on its own, with a hand-built grant and no socket, resolver or event
 * loop anywhere in scope. That is the point: the invariant is "denial happens BEFORE
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

#include "hull/cap/net_policy.h"

/* Build a grant of exactly the given hosts and ports, plus one login. The
 * default user keeps the reach cases about reach: a test that means to probe
 * the host rule should not also be asserting the user rule by accident. */
static void mf_init(HlManifestSsh *s)
{
    memset(s, 0, sizeof *s);
}

static void mf_allow(HlManifestSsh *s, const char *host, int port)
{
    s->declared = 1;
    s->connect.declared = 1;
    if (host) {
        s->connect.hosts[s->connect.host_count++] = host;
    }
    if (port) {
        s->connect.ports[s->connect.port_count++] = port;
    }
    if (s->user_count == 0) {
        s->users[s->user_count++] = "operator";
    }
}

/* ── the allow path ─────────────────────────────────────────────────── */

UTEST(net_policy, exact_host_and_port_allowed)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark-7468.local", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark-7468.local", 22, "operator"), (int)HL_NET_ALLOW);
}

UTEST(net_policy, suffix_glob_matches_subdomain)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*.local", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark-7468.local", 22, "operator"), (int)HL_NET_ALLOW);
}

UTEST(net_policy, host_match_is_case_insensitive)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark.local", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "SPARK.LOCAL", 22, "operator"), (int)HL_NET_ALLOW);
}

UTEST(net_policy, second_port_in_the_list_is_allowed)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "h.local", 22);
    g.connect.ports[g.connect.port_count++] = 2222;
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "h.local", 2222, "operator"), (int)HL_NET_ALLOW);
}

/* ── fail-closed: the three ways a policy can grant nothing ─────────── */

UTEST(net_policy, no_ssh_key_denies)
{
    HlManifestSsh g; mf_init(&g);          /* nothing declared at all */
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "operator"),
              (int)HL_NET_DENY_UNDECLARED);
}

UTEST(net_policy, ssh_declared_without_connect_denies)
{
    HlManifestSsh g; mf_init(&g);
    g.declared = 1;                 /* net = {} */
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "operator"),
              (int)HL_NET_DENY_NO_POLICY);
}

UTEST(net_policy, empty_host_list_is_not_a_wildcard)
{
    /* `ssh = { connect = { ports = {22} } }` reads like a grant and must not
     * behave like one. An absent host list is an empty grant. */
    HlManifestSsh g; mf_init(&g);
    g.declared = 1;
    g.connect.declared = 1;
    g.connect.ports[g.connect.port_count++] = 22;
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "operator"),
              (int)HL_NET_DENY_NO_POLICY);
}

UTEST(net_policy, empty_port_list_is_not_a_wildcard)
{
    /* Ports are required and never inferred, even though 22 is the obvious
     * intent of an SSH-shaped policy. */
    HlManifestSsh g; mf_init(&g);
    g.declared = 1;
    g.connect.declared = 1;
    g.connect.hosts[g.connect.host_count++] = "*.local";
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "operator"),
              (int)HL_NET_DENY_NO_POLICY);
}

/* ── fail-closed: destination outside the grant ─────────────────────── */

UTEST(net_policy, unlisted_host_denied)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark.local", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "evil.example.com", 22, "operator"),
              (int)HL_NET_DENY_HOST);
}

UTEST(net_policy, unlisted_port_denied)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark.local", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 2222, "operator"),
              (int)HL_NET_DENY_PORT);
}

UTEST(net_policy, suffix_glob_does_not_match_the_bare_suffix)
{
    /* "*.local" grants subdomains, not "local" itself. */
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*.local", 22);
    ASSERT_NE((int)hl_ssh_check_connect(&g, "local", 22, "operator"), (int)HL_NET_ALLOW);
}

UTEST(net_policy, suffix_glob_does_not_match_a_longer_sibling_domain)
{
    /* The classic allowlist bug: "*.local" must not admit "notlocal" or
     * "evil.local.attacker.com". */
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*.local", 22);
    ASSERT_NE((int)hl_ssh_check_connect(&g, "notlocal", 22, "operator"), (int)HL_NET_ALLOW);
    ASSERT_NE((int)hl_ssh_check_connect(&g, "spark.local.attacker.com", 22, "operator"),
              (int)HL_NET_ALLOW);
}

/* ── malformed input ────────────────────────────────────────────────── */

UTEST(net_policy, null_grant_denies)
{
    /* Reaching here without a grant is a programming error, and the safe
     * reading of a programming error is deny, not trust. */
    ASSERT_NE((int)hl_ssh_check_connect(NULL, "spark.local", 22, "operator"), (int)HL_NET_ALLOW);
}

UTEST(net_policy, null_or_empty_host_denies)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*", 22);              /* even under a wildcard */
    ASSERT_EQ((int)hl_ssh_check_connect(&g, NULL, 22, "operator"), (int)HL_NET_DENY_HOST);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "", 22, "operator"),   (int)HL_NET_DENY_HOST);
}

UTEST(net_policy, out_of_range_ports_denied)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "h", 0, "operator"),      (int)HL_NET_DENY_PORT);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "h", -1, "operator"),     (int)HL_NET_DENY_PORT);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "h", 65536, "operator"),  (int)HL_NET_DENY_PORT);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "h", 100000, "operator"), (int)HL_NET_DENY_PORT);
}

/* ── the wildcard is a real grant, and says so ──────────────────────── */

UTEST(net_policy, explicit_wildcard_host_allows_but_port_still_gates)
{
    /* "*" is a deliberate choice an app can make, and it still does not open
     * every port: the two lists are independent. */
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "anything.example.com", 22, "operator"),
              (int)HL_NET_ALLOW);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "anything.example.com", 23, "operator"),
              (int)HL_NET_DENY_PORT);
}


/* ── the login, which a reach grant cannot express ──────────────────── */

UTEST(ssh_policy, listed_user_allowed_unlisted_user_denied)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*.local", 22);        /* seeds "operator" */
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "operator"),
              (int)HL_NET_ALLOW);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "root"),
              (int)HL_NET_DENY_USER);
}

UTEST(ssh_policy, empty_user_list_is_not_a_wildcard)
{
    /* The whole point of the user rule: a reachable host must not imply any
     * login on it. */
    HlManifestSsh g; mf_init(&g);
    g.declared = 1;
    g.connect.declared = 1;
    g.connect.hosts[g.connect.host_count++] = "*.local";
    g.connect.ports[g.connect.port_count++] = 22;
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "operator"),
              (int)HL_NET_DENY_NO_POLICY);
}

UTEST(ssh_policy, user_match_is_case_sensitive)
{
    /* Unlike hosts. Unix logins are case-sensitive, and "Root" being a
     * different account from "root" is the whole reason to say so. */
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*.local", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "OPERATOR"),
              (int)HL_NET_DENY_USER);
}

UTEST(ssh_policy, null_or_empty_user_denies)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*.local", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, NULL),
              (int)HL_NET_DENY_USER);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, ""),
              (int)HL_NET_DENY_USER);
}

UTEST(ssh_policy, an_unreachable_host_is_refused_before_the_login_is_judged)
{
    /* A denial should name the broader problem. Telling someone their username
     * is wrong when they may not reach the machine at all sends them to fix
     * the wrong line. */
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark.local", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "evil.example.com", 22, "nobody"),
              (int)HL_NET_DENY_HOST);
}

UTEST(ssh_policy, a_second_listed_user_is_allowed)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "*.local", 22);
    g.users[g.user_count++] = "deploy";
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "deploy"),
              (int)HL_NET_ALLOW);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark.local", 22, "operator"),
              (int)HL_NET_ALLOW);
}

/* ── the generic reach check, used without SSH in scope ─────────────── */

UTEST(net_policy, the_reach_check_knows_nothing_about_users)
{
    /* hl_net_check_connect is the half a second protocol could reuse. It must
     * stay answerable from a host/port grant alone. */
    HlManifestNetConnect c;
    memset(&c, 0, sizeof c);
    c.declared = 1;
    c.hosts[c.host_count++] = "*.local";
    c.ports[c.port_count++] = 22;
    ASSERT_EQ((int)hl_net_check_connect(&c, "spark.local", 22), (int)HL_NET_ALLOW);
    ASSERT_EQ((int)hl_net_check_connect(&c, "spark.local", 23), (int)HL_NET_DENY_PORT);
    ASSERT_EQ((int)hl_net_check_connect(NULL, "spark.local", 22),
              (int)HL_NET_DENY_NO_POLICY);
}

/* ── the tunnel grant ────────────────────────────────────────────────── */

static void mf_tunnel(HlManifestSsh *s, const char *host, int port)
{
    s->tunnel.declared = 1;
    if (host) s->tunnel.hosts[s->tunnel.host_count++] = host;
    if (port) s->tunnel.ports[s->tunnel.port_count++] = port;
}

UTEST(net_policy, a_relay_needs_its_own_grant)
{
    /* THE case this gate exists for. An app allowed to SSH to a host is not
     * thereby allowed to route the connection through an arbitrary third
     * machine, because that machine terminates the TLS and sees the tunnel's
     * credentials. connect alone must not authorise a relay. */
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark-7468", 22);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark-7468", 22, "operator"),
              (int)HL_NET_ALLOW);
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "ssh.example.com", 443),
              (int)HL_NET_DENY_TUNNEL_UNDECLARED);
}

UTEST(net_policy, a_declared_relay_is_allowed_on_its_own_host_and_port)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark-7468", 22);
    mf_tunnel(&g, "ssh.example.com", 443);
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "ssh.example.com", 443),
              (int)HL_NET_ALLOW);
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "evil.example.com", 443),
              (int)HL_NET_DENY_TUNNEL_HOST);
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "ssh.example.com", 8443),
              (int)HL_NET_DENY_TUNNEL_PORT);
}

UTEST(net_policy, the_relay_grant_does_not_widen_which_host_may_be_reached)
{
    /* The whole reason the two lists are separate. The destination travels
     * inside the tunnel's headers and never appears in the socket address,
     * so if one list covered both, allowing the relay would have allowed SSH
     * to every host behind it. The destination check must be unmoved. */
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark-7468", 22);
    mf_tunnel(&g, "ssh.example.com", 443);
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "ssh.example.com", 443),
              (int)HL_NET_ALLOW);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "some-other-box", 22, "operator"),
              (int)HL_NET_DENY_HOST);
}

UTEST(net_policy, the_relay_grant_does_not_widen_which_login_may_be_used)
{
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark-7468", 22);
    mf_tunnel(&g, "ssh.example.com", 443);
    ASSERT_EQ((int)hl_ssh_check_connect(&g, "spark-7468", 22, "root"),
              (int)HL_NET_DENY_USER);
}

UTEST(net_policy, an_empty_relay_grant_is_not_a_wildcard)
{
    /* `tunnel = {}` reads like "allow tunnels" and must not behave like it,
     * the same rule connect already follows. */
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark-7468", 22);
    g.tunnel.declared = 1;                       /* declared, but empty */
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "ssh.example.com", 443),
              (int)HL_NET_DENY_TUNNEL_NO_POLICY);

    /* Hosts but no ports is equally empty: both lists are required. */
    g.tunnel.hosts[g.tunnel.host_count++] = "ssh.example.com";
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "ssh.example.com", 443),
              (int)HL_NET_DENY_TUNNEL_NO_POLICY);
}

UTEST(net_policy, the_relay_check_fails_closed_without_a_policy)
{
    HlManifestSsh g; mf_init(&g);
    ASSERT_EQ((int)hl_ssh_check_tunnel(NULL, "ssh.example.com", 443),
              (int)HL_NET_DENY_NO_POLICY);
    /* No `ssh` key at all is a different answer from `ssh` without a tunnel:
     * the first says add the capability, the second says add the relay. */
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "ssh.example.com", 443),
              (int)HL_NET_DENY_UNDECLARED);
}

UTEST(net_policy, the_relay_host_rule_uses_the_same_matcher)
{
    /* Globs and CIDRs work here for the same reason they work for connect -
     * it is one matcher, shared with http / ws / smtp / databases.dynamic. */
    HlManifestSsh g; mf_init(&g);
    mf_allow(&g, "spark-7468", 22);
    mf_tunnel(&g, "*.example.com", 443);
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "ssh.example.com", 443),
              (int)HL_NET_ALLOW);
    ASSERT_EQ((int)hl_ssh_check_tunnel(&g, "example.com", 443),
              (int)HL_NET_DENY_TUNNEL_HOST);
}

/* ── denial reasons are distinguishable ─────────────────────────────── */

UTEST(net_policy, every_denial_has_its_own_reason_string)
{
    /* The reasons exist so a message can say WHICH rule refused. If they
     * collapse to one string the distinction is decorative. */
    const char *r[10];
    r[0] = hl_cap_net_auth_reason(HL_NET_ALLOW);
    r[1] = hl_cap_net_auth_reason(HL_NET_DENY_UNDECLARED);
    r[2] = hl_cap_net_auth_reason(HL_NET_DENY_NO_POLICY);
    r[3] = hl_cap_net_auth_reason(HL_NET_DENY_HOST);
    r[4] = hl_cap_net_auth_reason(HL_NET_DENY_PORT);
    r[5] = hl_cap_net_auth_reason(HL_NET_DENY_USER);
    /* The tunnel reasons included: a message that says "host is not in
     * ssh.connect.hosts" when the RELAY was refused sends the reader to the
     * wrong list, which is worse than saying nothing. */
    r[6] = hl_cap_net_auth_reason(HL_NET_DENY_TUNNEL_UNDECLARED);
    r[7] = hl_cap_net_auth_reason(HL_NET_DENY_TUNNEL_NO_POLICY);
    r[8] = hl_cap_net_auth_reason(HL_NET_DENY_TUNNEL_HOST);
    r[9] = hl_cap_net_auth_reason(HL_NET_DENY_TUNNEL_PORT);
    for (int i = 0; i < 10; i++) {
        ASSERT_NE(r[i], NULL);
        ASSERT_GT(strlen(r[i]), (size_t)0);
        for (int j = i + 1; j < 10; j++)
            ASSERT_NE(strcmp(r[i], r[j]), 0);
    }
}

UTEST_MAIN()
