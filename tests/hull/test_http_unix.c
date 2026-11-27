/*
 * test_http_unix.c - manifest http.unix_sockets: the entry shape, its
 * resolution, the dial-time path check and the URL rewrite
 * (hull/shared/http_unix.h, #1228).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/shared/http_unix.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

UTEST(http_unix, entry_shape)
{
    char why[64];
    EXPECT_EQ(1, hl_http_unix_entry_valid("run/svc.sock", why, sizeof why));
    EXPECT_EQ(1, hl_http_unix_entry_valid("@state/infer.sock", why, sizeof why));
    EXPECT_EQ(1, hl_http_unix_entry_valid("/run/inkleberry.sock", why, sizeof why));
    EXPECT_EQ(1, hl_http_unix_entry_valid("a.sock", why, sizeof why));

    const char *bad[] = {
        "", "../x.sock", "run/../x.sock", "./x.sock", "run//x.sock", "run/",
        "/", "//x.sock", "/run/./x.sock", "/a/../b.sock", ".hull/x.sock",
        ".HULL/x.sock", "@state/", "@other/x.sock", "@state/../x", "a\\b.sock",
        "a\nb.sock",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        EXPECT_EQ(0, hl_http_unix_entry_valid(bad[i], why, sizeof why));

    char longp[HL_HTTP_UNIX_PATH_MAX + 4];
    memset(longp, 'a', sizeof longp - 1);
    longp[sizeof longp - 1] = '\0';
    EXPECT_EQ(0, hl_http_unix_entry_valid(longp, why, sizeof why));
    EXPECT_EQ(0, hl_http_unix_entry_valid(NULL, why, sizeof why));
}

typedef struct { char root[64]; char app[96]; char state[96]; } Dirs;

static int make_dirs(Dirs *d)
{
    snprintf(d->root, sizeof d->root, "/tmp/hut.XXXXXX");
    if (!mkdtemp(d->root)) return -1;
    char real[PATH_MAX];
    if (!realpath(d->root, real) || strlen(real) >= sizeof d->root) return -1;
    snprintf(d->root, sizeof d->root, "%s", real);
    snprintf(d->app, sizeof d->app, "%s/app", d->root);
    snprintf(d->state, sizeof d->state, "%s/st", d->root);
    if (mkdir(d->app, 0700) != 0 || mkdir(d->state, 0700) != 0) return -1;
    char run[128];
    snprintf(run, sizeof run, "%s/run", d->app);
    return mkdir(run, 0700);
}

static void rm_dirs(const Dirs *d)
{
    char p[160];
    const char *names[] = { "app/run/svc.sock", "app/run/link.sock",
                            "app/run/file.sock", "app/run/sub", "app/lnk",
                            "app/run", "app", "st/s.sock", "st" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", d->root, names[i]);
        if (unlink(p) != 0) rmdir(p);
    }
    rmdir(d->root);
}

static int bind_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int rc = bind(fd, (struct sockaddr *)&a, sizeof a);
    close(fd);
    return rc;
}

UTEST(http_unix, resolve_and_find)
{
    Dirs d;
    ASSERT_EQ(0, make_dirs(&d));
    const char *e[] = { "run/svc.sock", "@state/s.sock", "/run/x.sock" };
    HlHttpUnixSet set;
    char err[256];
    ASSERT_EQ(0, hl_http_unix_resolve(&set, e, 3, d.app, d.state, err, sizeof err));
    EXPECT_EQ(3, set.count);
    char want[160];
    snprintf(want, sizeof want, "%s/run/svc.sock", d.app);
    EXPECT_STREQ(want, set.resolved[0]);
    snprintf(want, sizeof want, "%s/s.sock", d.state);
    EXPECT_STREQ(want, set.resolved[1]);
    EXPECT_STREQ("/run/x.sock", set.resolved[2]);
    EXPECT_EQ((int)strlen(d.app), (int)set.root_len[0]);
    EXPECT_EQ(0, (int)set.root_len[2]);

    /* Found by its spelling and by its resolved path; nothing else. */
    EXPECT_EQ(0, hl_http_unix_find(&set, "run/svc.sock"));
    EXPECT_EQ(0, hl_http_unix_find(&set, set.resolved[0]));
    EXPECT_EQ(1, hl_http_unix_find(&set, "@state/s.sock"));
    EXPECT_EQ(2, hl_http_unix_find(&set, "/run/x.sock"));
    EXPECT_EQ(-1, hl_http_unix_find(&set, "run/other.sock"));
    EXPECT_EQ(-1, hl_http_unix_find(&set, "/run/x.sock/"));
    EXPECT_EQ(-1, hl_http_unix_find(&set, NULL));

    /* "@state/" with no state directory, a bad entry, a path too long. */
    EXPECT_EQ(-1, hl_http_unix_resolve(&set, e + 1, 1, d.app, NULL, err, sizeof err));
    EXPECT_TRUE(strstr(err, "state directory") != NULL);
    const char *bad[] = { "../x.sock" };
    EXPECT_EQ(-1, hl_http_unix_resolve(&set, bad, 1, d.app, NULL, err, sizeof err));
    char longrel[HL_HTTP_UNIX_PATH_MAX];
    memset(longrel, 'a', HL_HTTP_UNIX_PATH_MAX - 20);
    longrel[HL_HTTP_UNIX_PATH_MAX - 20] = '\0';
    const char *lr[] = { longrel };
    EXPECT_EQ(-1, hl_http_unix_resolve(&set, lr, 1, d.app, NULL, err, sizeof err));
    EXPECT_TRUE(strstr(err, "longer") != NULL);
    rm_dirs(&d);
}

UTEST(http_unix, path_ok_refuses_links_and_non_sockets)
{
    Dirs d;
    ASSERT_EQ(0, make_dirs(&d));
    char p[160], q[160];
    snprintf(p, sizeof p, "%s/run/svc.sock", d.app);
    ASSERT_EQ(0, bind_socket(p));
    snprintf(p, sizeof p, "%s/run/link.sock", d.app);
    ASSERT_EQ(0, symlink("svc.sock", p));
    snprintf(p, sizeof p, "%s/run/file.sock", d.app);
    FILE *f = fopen(p, "w");
    ASSERT_TRUE(f != NULL);
    fclose(f);
    snprintf(p, sizeof p, "%s/run", d.app);
    snprintf(q, sizeof q, "%s/lnk", d.app);
    ASSERT_EQ(0, symlink(p, q));

    const char *e[] = { "run/svc.sock", "run/link.sock", "run/file.sock",
                        "run/missing.sock", "lnk/svc.sock" };
    HlHttpUnixSet set;
    char err[256];
    ASSERT_EQ(0, hl_http_unix_resolve(&set, e, 5, d.app, NULL, err, sizeof err));
    EXPECT_EQ(1, hl_http_unix_path_ok(&set, 0));   /* a socket */
    EXPECT_EQ(0, hl_http_unix_path_ok(&set, 1));   /* the leaf is a link */
    EXPECT_EQ(0, hl_http_unix_path_ok(&set, 2));   /* not a socket */
    EXPECT_EQ(1, hl_http_unix_path_ok(&set, 3));   /* absent: connect fails */
    EXPECT_EQ(0, hl_http_unix_path_ok(&set, 4));   /* a link below the root */
    EXPECT_EQ(0, hl_http_unix_path_ok(&set, 5));   /* out of range */

    /* Absolute: the leaf alone is checked. */
    char abs0[160], abs1[160];
    snprintf(abs0, sizeof abs0, "%s/run/svc.sock", d.app);
    snprintf(abs1, sizeof abs1, "%s/run/link.sock", d.app);
    const char *a[] = { abs0, abs1 };
    ASSERT_EQ(0, hl_http_unix_resolve(&set, a, 2, d.app, NULL, err, sizeof err));
    EXPECT_EQ(1, hl_http_unix_path_ok(&set, 0));
    EXPECT_EQ(0, hl_http_unix_path_ok(&set, 1));
    rm_dirs(&d);
}

UTEST(http_unix, rewrite_keeps_the_target)
{
    const char *e[] = { "/r/a b.sock" };
    HlHttpUnixSet set;
    char err[256];
    ASSERT_EQ(0, hl_http_unix_resolve(&set, e, 1, NULL, NULL, err, sizeof err));
    char *u = hl_http_unix_rewrite(&set, 0, "http+unix://%2Fr%2Fa%20b.sock/v1/x?q=1#f");
    ASSERT_TRUE(u != NULL);
    EXPECT_STREQ("http+unix://%2Fr%2Fa%20b.sock/v1/x?q=1#f", u);
    free(u);
    u = hl_http_unix_rewrite(&set, 0, "https+unix://anything");
    ASSERT_TRUE(u != NULL);
    EXPECT_STREQ("https+unix://%2Fr%2Fa%20b.sock", u);
    free(u);
    EXPECT_TRUE(hl_http_unix_rewrite(&set, 0, "no-scheme") == NULL);
    EXPECT_TRUE(hl_http_unix_rewrite(&set, 1, "http+unix://x/") == NULL);
}

UTEST_MAIN();
