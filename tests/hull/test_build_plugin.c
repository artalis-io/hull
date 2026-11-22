/*
 * test_build_plugin.c - the plugin side of a build plugin's channel
 * (shared/build_plugin.c): the manifest preset, section by section and
 * module by module, taking the inherited descriptor, and leaving the
 * plugin directories hull.build.json names out of an app's file list.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/shared/build_plugin.h"
#include "test_tmpdir.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void base(HlManifest *m)
{
    memset(m, 0, sizeof *m);
    m->modules[0].name = "hull/build-plugin";
    m->modules[1].name = "hull/json";
    m->modules_count = 2;
}

UTEST(build_plugin, preset_admits_pure_modules)
{
    HlManifest m;
    char err[256];
    base(&m);
    ASSERT_EQ(0, hl_build_plugin_preset_check(&m, err, sizeof err));
    ASSERT_TRUE(hl_build_plugin_module_allowed("hull/template@1"));
    ASSERT_TRUE(hl_build_plugin_module_allowed("hull/app"));
    static const char *const refused[] = {
        "hull/time", "hull/crypto", "hull/uuid", "hull/fs", "hull/db",
        "hull/http-client", "hull/env", "hull/ssh", "hull/kv", "hull/tui",
        "hull/timers", "hull/compute", "hull/build-pluginx", "hull/jso", NULL,
    };
    for (int i = 0; refused[i]; i++) {
        ASSERT_FALSE(hl_build_plugin_module_allowed(refused[i]));
        base(&m);
        m.modules[2].name = refused[i];
        m.modules_count = 3;
        ASSERT_EQ(-1, hl_build_plugin_preset_check(&m, err, sizeof err));
        ASSERT_TRUE(strstr(err, refused[i]) != NULL);
    }
}

UTEST(build_plugin, preset_refuses_every_section)
{
    HlManifest m;
    char err[256];
    static const char *const want[] = {
        "`fs`", "`fs`", "`env`", "`secrets`", "`hosts`", "`databases`",
        "`kv`", "`ssh`", "`tui`", "`state`", "`gpu`", "`compute`",
    };
    for (int c = 0; c < 12; c++) {
        base(&m);
        switch (c) {
        case 0:  m.fs_read_count = 1; break;
        case 1:  m.fs_write_count = 1; break;
        case 2:  m.env_count = 1; break;
        case 3:  m.secrets_count = 1; break;
        case 4:  m.hosts_count = 1; break;
        case 5:  m.databases.declared = 1; break;
        case 6:  m.kv.declared = 1; break;
        case 7:  m.ssh.declared = 1; break;
        case 8:  m.tui = 1; break;
        case 9:  m.state_name = "x"; break;
        case 10: m.gpu = 1; break;
        default: m.compute = 1; break;
        }
        ASSERT_EQ(-1, hl_build_plugin_preset_check(&m, err, sizeof err));
        ASSERT_TRUE(strstr(err, want[c]) != NULL);
    }
}

UTEST(build_plugin, preset_requires_the_module)
{
    HlManifest m;
    char err[256];
    memset(&m, 0, sizeof m);
    m.modules[0].name = "hull/json";
    m.modules_count = 1;
    ASSERT_EQ(-1, hl_build_plugin_preset_check(&m, err, sizeof err));
    ASSERT_TRUE(strstr(err, "hull/build-plugin@1") != NULL);
}

#ifdef HL_CHAN_SUPPORTED
#include <fcntl.h>

/* A descriptor moved above 10, so placing it on fd 3 never closes another
 * of the test's own (pipe / socketpair may hand out 3 itself). */
static int high(int fd)
{
    int h = fcntl(fd, F_DUPFD, 10);
    close(fd);
    return h;
}

UTEST(build_plugin, take_env)
{
    if (!hl_chan_host_ok()) UTEST_SKIP("no channel child on Windows");
    unsetenv(HL_BUILD_PLUGIN_ENV);
    ASSERT_EQ(0, hl_build_plugin_take_env());            /* not a plugin */
    ASSERT_EQ(-1, hl_build_plugin_fd());

    setenv(HL_BUILD_PLUGIN_ENV, "4", 1);                 /* only fd 3 */
    ASSERT_EQ(-1, hl_build_plugin_take_env());
    ASSERT_TRUE(getenv(HL_BUILD_PLUGIN_ENV) == NULL);    /* removed anyway */

    int saved = fcntl(3, F_DUPFD, 10);                   /* fd 3 a pipe */
    int p[2];
    ASSERT_EQ(0, pipe(p));
    p[0] = high(p[0]);
    p[1] = high(p[1]);
    ASSERT_EQ(3, dup2(p[0], 3));
    setenv(HL_BUILD_PLUGIN_ENV, "3", 1);
    ASSERT_EQ(-1, hl_build_plugin_take_env());           /* not a socket */
    close(p[0]);
    close(p[1]);

    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    sv[0] = high(sv[0]);
    sv[1] = high(sv[1]);
    ASSERT_EQ(3, dup2(sv[1], 3));
    setenv(HL_BUILD_PLUGIN_ENV, "3", 1);
    ASSERT_EQ(1, hl_build_plugin_take_env());
    ASSERT_EQ(3, hl_build_plugin_fd());
    ASSERT_TRUE(getenv(HL_BUILD_PLUGIN_ENV) == NULL);

    /* A frame each way, then the end of the channel. */
    ASSERT_EQ(0, hl_build_plugin_send(HL_BP_CALL, 5, "\x00", 1, hl_chan_mono_ms() + 1000));
    HlChanFrame f = { .magic = HL_BUILD_PLUGIN_MAGIC, .max = 16 };
    hl_chan_set_nonblock(sv[0]);
    ASSERT_EQ(HL_CHAN_DONE, hl_chan_frame_read(sv[0], &f, hl_chan_mono_ms() + 1000));
    ASSERT_EQ(HL_BP_CALL, f.type);
    ASSERT_EQ(5u, f.id);
    hl_chan_frame_reset(&f);
    close(sv[0]);
    close(sv[1]);                    /* fd 3 is the plugin's end now */
    ASSERT_EQ(HL_CHAN_EOF, hl_build_plugin_recv(&f, hl_chan_mono_ms() + 1000));
    hl_chan_frame_reset(&f);
    ASSERT_EQ(-1, hl_build_plugin_fd());                 /* ended */
    ASSERT_EQ(-1, hl_build_plugin_send(HL_BP_DONE, 0, NULL, 0, 0));
    if (saved >= 0) { dup2(saved, 3); close(saved); }
}
#endif

static void write_text(const char *dir, const char *name, const char *text)
{
    char p[HL_TEST_PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static char **list_of(const char *dir, const char *const *rel, size_t n)
{
    char **out = calloc(n + 1, sizeof *out);
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(dir) + strlen(rel[i]) + 2;
        out[i] = malloc(len);
        snprintf(out[i], len, "%s/%s", dir, rel[i]);
    }
    return out;
}

static void free_list(char **l)
{
    for (char **p = l; *p; p++) free(*p);
    free(l);
}

UTEST(build_plugin, drop_dirs_leaves_plugins_out)
{
    char dir[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(dir, sizeof dir, "hull_bp_drop") != NULL);
    static const char *const rel[] = {
        "tests/test_app.lua", "plugins/inv/tests/test_inv.lua", "plugins/inventory/test_x.lua",
        "plugins/inv", "lib/plugins/inv/test_y.lua",
    };
    char **l = list_of(dir, rel, 5);
    hl_build_plugin_drop_dirs(dir, l);           /* no hull.build.json: nothing dropped */
    int n = 0;
    for (char **p = l; *p; p++) n++;
    EXPECT_EQ(n, 5);
    free_list(l);

    write_text(dir, "hull.build.json", "{\"plugins\":[{\"path\":\"plugins/inv\"},{\"path\":7}]}");
    l = list_of(dir, rel, 5);
    hl_build_plugin_drop_dirs(dir, l);
    n = 0;
    for (char **p = l; *p; p++) n++;
    ASSERT_EQ(n, 4);                              /* only plugins/inv/... went */
    char want[HL_TEST_PATH_MAX + 64];
    snprintf(want, sizeof want, "%s/plugins/inventory/test_x.lua", dir);
    EXPECT_STREQ(l[1], want);                     /* a longer sibling name stays */
    EXPECT_TRUE(l[4] == NULL);
    free_list(l);

    write_text(dir, "hull.build.json", "not json");
    l = list_of(dir, rel, 5);
    hl_build_plugin_drop_dirs(dir, l);            /* unreadable: nothing dropped */
    n = 0;
    for (char **p = l; *p; p++) n++;
    EXPECT_EQ(n, 5);
    free_list(l);

    char p[HL_TEST_PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/hull.build.json", dir);
    remove(p);
    rmdir(dir);
}

UTEST_MAIN()
