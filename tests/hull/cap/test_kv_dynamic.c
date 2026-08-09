/*
 * test_kv_dynamic.c: the kv.open(dsn) allowlist validator (hl_cap_kv_check_dsn).
 *
 * Pure C, no server: builds a HlManifestKvDynamic policy and checks that scheme
 * + host gating admits/denies the right DSNs, that a NULL / undeclared / empty
 * policy fails closed, and that "$VAR" env-ref hosts resolve.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/kv.h"
#include "hull/manifest.h"
#include <stdlib.h>
#include <string.h>

static HlManifestKvDynamic policy_of(const char *const *schemes, int ns,
                                     const char *const *hosts, int nh)
{
    HlManifestKvDynamic p;
    memset(&p, 0, sizeof p);
    p.declared = 1;
    for (int i = 0; i < ns; i++) p.schemes[i] = schemes[i];
    p.scheme_count = ns;
    for (int i = 0; i < nh; i++) p.hosts[i] = hosts[i];
    p.host_count = nh;
    return p;
}

#define OK(dsn, p)   ASSERT_EQ(0,  hl_cap_kv_check_dsn((p), (dsn), e, sizeof e))
#define DENY(dsn, p) ASSERT_EQ(-1, hl_cap_kv_check_dsn((p), (dsn), e, sizeof e))

UTEST(kv_dynamic, scheme_and_host_gate) {
    char e[256];
    const char *schemes[] = { "redis", "valkey" };
    const char *hosts[]   = { "127.0.0.1", "*.example.com" };
    HlManifestKvDynamic p = policy_of(schemes, 2, hosts, 2);

    OK("redis://127.0.0.1:6379", &p);
    OK("valkey://cache.example.com", &p);          /* subdomain glob */
    OK("redis://:secret@127.0.0.1:6379/0", &p);    /* userinfo stripped */
    DENY("redis://10.0.0.1", &p);                  /* host not allowed */
    DENY("mysql://127.0.0.1", &p);                 /* scheme not allowed */
    DENY("rediss://127.0.0.1", &p);                /* rediss not in schemes */
    DENY("redis://example.com", &p);               /* glob is *.example.com, not bare */
}

UTEST(kv_dynamic, fails_closed) {
    char e[256];
    const char *schemes[] = { "redis" };
    const char *hosts[]   = { "127.0.0.1" };

    /* NULL policy denies. */
    DENY("redis://127.0.0.1", NULL);

    /* Undeclared policy denies. */
    HlManifestKvDynamic undeclared;
    memset(&undeclared, 0, sizeof undeclared);
    DENY("redis://127.0.0.1", &undeclared);

    /* Declared but empty schemes denies every scheme. */
    HlManifestKvDynamic empty = policy_of(NULL, 0, hosts, 1);
    DENY("redis://127.0.0.1", &empty);

    /* Declared, scheme ok, but empty hosts denies every host. */
    HlManifestKvDynamic nohost = policy_of(schemes, 1, NULL, 0);
    DENY("redis://127.0.0.1", &nohost);

    /* Empty / scheme-less DSNs. */
    HlManifestKvDynamic p = policy_of(schemes, 1, hosts, 1);
    DENY("", &p);
    DENY("127.0.0.1:6379", &p);   /* no scheme */
}

UTEST(kv_dynamic, env_ref_host) {
    char e[256];
    setenv("HULL_TEST_KV_HOST", "10.9.8.7", 1);
    const char *schemes[] = { "valkey" };
    const char *hosts[]   = { "$HULL_TEST_KV_HOST" };
    HlManifestKvDynamic p = policy_of(schemes, 1, hosts, 1);

    OK("valkey://10.9.8.7:6379", &p);
    DENY("valkey://10.9.8.8:6379", &p);
    unsetenv("HULL_TEST_KV_HOST");
}

UTEST_MAIN();
