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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hull/cap/ssh.h"
#include "hull/shared/async_backend.h"

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

/* Audit 21: the relay's TLS name (its SNI, certificate name and the
 * upgrade's Host) is what a CDN routes by, so it needs the grant too. */
UTEST(cap_ssh, a_relay_name_outside_the_grant_is_denied)
{
    HlManifestSsh g; grant(&g);
    g.tunnel.declared = 1;
    g.tunnel.hosts[g.tunnel.host_count++] = "ssh.example.com";
    g.tunnel.ports[g.tunnel.port_count++] = 443;
    HlSshEnv env = { .policy = &g };
    HlSshOpen r = direct();
    r.via = 1;
    r.via_host = "ssh.example.com";
    r.via_port = 443;
    r.via_tls = 1;
    r.via_tls_hostname = "other.zone.example";
    HlNetStream *s = NULL;
    const char *why = NULL;
    ASSERT_EQ(hl_cap_ssh_open(&env, &r, &s, &why), HL_NET_E_DENIED);
    ASSERT_TRUE(s == NULL);
    ASSERT_STREQ(why, "tunnel host is not in ssh.tunnel.hosts");
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

/* ── live-stream cap (audit 35) ─────────────────────────────────────── */

/* A loopback listener that accepts nothing: a connect completes from the
 * backlog, and an un-pumped one stays in flight - either way a live stream
 * holding a socket, which is what the cap counts. */
static int listen_loopback(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof a;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 ||
        listen(fd, 128) < 0 ||
        getsockname(fd, (struct sockaddr *)&a, &alen) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return fd;
}

UTEST(cap_ssh, live_streams_are_capped_and_a_freed_stream_gives_its_slot_back)
{
    /* The backend hl_cap_ssh_open schedules on: the one this binary links. */
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    HlAsyncBackendCtx  *ctx  = NULL;
    HlAsyncBackendPool *pool = NULL;
    ASSERT_EQ(be->init(&ctx, NULL), 0);
    ASSERT_EQ(be->pool_create(&pool, ctx, 2, 128), 0);
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT_GE(lfd, 0);

    HlManifestSsh g;
    memset(&g, 0, sizeof g);
    g.declared = 1;
    g.connect.declared = 1;
    g.connect.hosts[g.connect.host_count++] = "127.0.0.1";
    g.connect.ports[g.connect.port_count++] = port;
    g.users[g.user_count++] = "operator";
    HlSshEnv env = { .policy = &g, .async = ctx, .pool = pool };
    HlSshOpen r = direct();
    r.host = "127.0.0.1";
    r.port = port;

    ASSERT_EQ(hl_cap_ssh_live_streams(), 0);
    HlNetStream *s[HL_SSH_MAX_LIVE_STREAMS];
    for (int i = 0; i < HL_SSH_MAX_LIVE_STREAMS; i++) {
        s[i] = NULL;
        int rc = hl_cap_ssh_open(&env, &r, &s[i], NULL);
        ASSERT_TRUE(rc == HL_NET_OK || rc == HL_NET_E_AGAIN);
        ASSERT_TRUE(s[i] != NULL);
    }
    ASSERT_EQ(hl_cap_ssh_live_streams(), HL_SSH_MAX_LIVE_STREAMS);

    /* One past the cap: refused before anything is dialled. */
    HlNetStream *extra = (HlNetStream *)1;
    const char *why = NULL;
    ASSERT_EQ(hl_cap_ssh_open(&env, &r, &extra, &why), HL_NET_E_LIMIT);
    ASSERT_TRUE(extra == NULL);
    ASSERT_TRUE(why != NULL && strstr(why, "too many open SSH connections"));
    ASSERT_EQ(hl_cap_ssh_live_streams(), HL_SSH_MAX_LIVE_STREAMS);

    /* A denial costs no slot even at the cap (the grant is checked first). */
    HlSshOpen bad = r;
    bad.user = "root";
    why = NULL;
    ASSERT_EQ(hl_cap_ssh_open(&env, &bad, &extra, &why), HL_NET_E_DENIED);

    /* Free one; its slot comes back once the transport lets go of it (a
     * connect still resolving releases from the loop). */
    hl_net_stream_close(s[0]);
    hl_net_stream_free(s[0]);
    s[0] = NULL;
    for (int i = 0; i < 200 &&
         hl_cap_ssh_live_streams() == HL_SSH_MAX_LIVE_STREAMS; i++)
        be->tick(ctx, 10);
    ASSERT_EQ(hl_cap_ssh_live_streams(), HL_SSH_MAX_LIVE_STREAMS - 1);

    int rc = hl_cap_ssh_open(&env, &r, &s[0], NULL);
    ASSERT_TRUE(rc == HL_NET_OK || rc == HL_NET_E_AGAIN);
    ASSERT_TRUE(s[0] != NULL);

    for (int i = 0; i < HL_SSH_MAX_LIVE_STREAMS; i++) {
        hl_net_stream_close(s[i]);
        hl_net_stream_free(s[i]);
    }
    for (int i = 0; i < 200 && hl_cap_ssh_live_streams() > 0; i++)
        be->tick(ctx, 10);
    EXPECT_EQ(hl_cap_ssh_live_streams(), 0);

    close(lfd);
    be->pool_free(pool);
    be->free(ctx);
}

UTEST_MAIN()
