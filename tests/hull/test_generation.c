/*
 * test_generation.c - a pipeline generation handed to an app run from
 * source (shared/generation.c) and the loaders' overlay it installs
 * (vfs.c): the bundle's checks, the names an entry may have, the
 * descriptor hand-off, and the refusals (a built binary ignores one,
 * --verify-sig refuses one).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/cap/crypto.h"
#include "hull/shared/generation.h"
#include "hull/shared/app_entry.h"
#include "hull/vfs.h"
#include "../../src/hull/utils/hex.h"
#include "test_tmpdir.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *name;
    const char *data;
    const char *hash;      /* NULL: the payload's real hash */
    const char *extra;     /* extra JSON members, or NULL */
} Ent;

/* Build a bundle: header, manifest, payloads. `size_delta` is added to the
 * last entry's declared size; `trailer` is appended after the payloads. */
static size_t build(uint8_t *out, size_t cap, const char *head_members,
                    const Ent *e, size_t n, int size_delta,
                    const char *trailer)
{
    char man[8192];
    size_t m = 0;
    m += (size_t)snprintf(man + m, sizeof man - m,
        "{%s\"entries\":[", head_members
            ? head_members
            : "\"format\":\"hull-pipeline-generation\",\"version\":1,"
              "\"generation_id\":\"t\",");
    for (size_t i = 0; i < n; i++) {
        char hex[65];
        uint8_t d[32];
        size_t len = strlen(e[i].data);
        hl_cap_crypto_sha256(e[i].data, len, d);
        hl_hex_encode(d, 32, hex, sizeof hex);
        int sz = (int)len + (i + 1 == n ? size_delta : 0);
        m += (size_t)snprintf(man + m, sizeof man - m,
            "%s{\"vfs_name\":\"%s\",\"content_hash\":\"%s\",\"size\":%d%s%s}",
            i ? "," : "", e[i].name, e[i].hash ? e[i].hash : hex, sz,
            e[i].extra ? "," : "", e[i].extra ? e[i].extra : "");
    }
    m += (size_t)snprintf(man + m, sizeof man - m, "],\"inputs\":[]}");
    size_t o = (size_t)snprintf((char *)out, cap, HL_GENERATION_MAGIC "%zu\n%s",
                                m, man);
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(e[i].data);
        memcpy(out + o, e[i].data, len);
        o += len;
    }
    if (trailer) {
        memcpy(out + o, trailer, strlen(trailer));
        o += strlen(trailer);
    }
    return o;
}

static int install(const uint8_t *b, size_t n, char *err)
{
    hl_vfs_overlay_reset();
    err[0] = '\0';
    return hl_generation_install(b, n, err, 256);
}

UTEST(generation, installs_and_shadows)
{
    static uint8_t b[16384];
    char err[256];
    const Ent e[] = {
        { "app.lua", "return 1\n", NULL, "\"position_map\":\"identity\"" },
        { "lib/m.lua", "return { v = 2 }\n", NULL,
          "\"origin\":{\"source\":\"lib/m.ts\"}" },
        { "compute/k.wasm", "", NULL, "\"position_map\":\"none\"" },
    };
    size_t n = build(b, sizeof b, NULL, e, 3, 0, NULL);
    ASSERT_EQ(0, install(b, n, err));
    ASSERT_TRUE(hl_vfs_overlay_active());
    const HlEntry *h = hl_vfs_overlay_find("lib/m.lua");
    ASSERT_TRUE(h != NULL);
    ASSERT_EQ((unsigned)strlen(e[1].data), h->len);
    ASSERT_EQ(0, memcmp(h->data, e[1].data, h->len));
    ASSERT_EQ(0, h->data[h->len]);              /* NUL after the payload */
    ASSERT_TRUE(hl_vfs_overlay_find("./lib/m.lua") == h);
    h = hl_vfs_overlay_find("compute/k.wasm");
    ASSERT_TRUE(h != NULL);
    ASSERT_EQ(0u, h->len);
    ASSERT_TRUE(hl_vfs_overlay_find("lib/x.lua") == NULL);
    ASSERT_TRUE(hl_vfs_overlay_find("lib") == NULL);
    /* Once per process: a second install is refused. */
    ASSERT_EQ(-1, hl_generation_install(b, n, err, sizeof err));
    hl_vfs_overlay_reset();
    ASSERT_FALSE(hl_vfs_overlay_active());
    ASSERT_TRUE(hl_vfs_overlay_find("lib/m.lua") == NULL);
}

UTEST(generation, empty_generation_installs)
{
    static uint8_t b[4096];
    char err[256];
    size_t n = build(b, sizeof b, NULL, NULL, 0, 0, NULL);
    ASSERT_EQ(0, install(b, n, err));
    ASSERT_TRUE(hl_vfs_overlay_active());
    ASSERT_TRUE(hl_vfs_overlay_find("app.lua") == NULL);
    hl_vfs_overlay_reset();
}

UTEST(generation, refuses_a_payload_that_does_not_match)
{
    static uint8_t b[4096];
    char err[256];
    const Ent e[] = { { "app.lua", "return 1\n", NULL, NULL } };
    size_t n = build(b, sizeof b, NULL, e, 1, 0, NULL);
    b[n - 2] ^= 1;                               /* the payload changed */
    ASSERT_EQ(-1, install(b, n, err));
    ASSERT_TRUE(strstr(err, "content_hash") != NULL);
    ASSERT_FALSE(hl_vfs_overlay_active());

    /* An upper-case hash is not the canonical spelling. */
    char up[65];
    uint8_t d[32];
    hl_cap_crypto_sha256("return 1\n", 9, d);
    hl_hex_encode(d, 32, up, sizeof up);
    for (char *c = up; *c; c++)
        if (*c >= 'a' && *c <= 'f') *c = (char)(*c - 'a' + 'A');
    const Ent u[] = { { "app.lua", "return 1\n", up, NULL } };
    n = build(b, sizeof b, NULL, u, 1, 0, NULL);
    ASSERT_EQ(-1, install(b, n, err));
    const Ent s[] = { { "app.lua", "return 1\n", "abc", NULL } };
    n = build(b, sizeof b, NULL, s, 1, 0, NULL);
    ASSERT_EQ(-1, install(b, n, err));
}

UTEST(generation, refuses_bad_framing)
{
    static uint8_t b[4096];
    char err[256];
    const Ent e[] = { { "app.lua", "return 1\n", NULL, NULL } };
    size_t n;

    /* Bytes past the last payload, and a payload short of its size. */
    n = build(b, sizeof b, NULL, e, 1, 0, "x");
    ASSERT_EQ(-1, install(b, n, err));
    n = build(b, sizeof b, NULL, e, 1, 1, NULL);
    ASSERT_EQ(-1, install(b, n, err));
    n = build(b, sizeof b, NULL, e, 1, -1, NULL);
    ASSERT_EQ(-1, install(b, n, err));

    /* The header. */
    n = build(b, sizeof b, NULL, e, 1, 0, NULL);
    ASSERT_EQ(-1, install(b, 10, err));
    uint8_t c[4096];
    memcpy(c, b, n);
    c[0] = 'H';
    ASSERT_EQ(-1, install(c, n, err));
    static const char *const heads[] = {
        HL_GENERATION_MAGIC "\n{}", HL_GENERATION_MAGIC "02\n{}",
        HL_GENERATION_MAGIC "2 \n{}", HL_GENERATION_MAGIC "9\n{}",
        HL_GENERATION_MAGIC "0\n", HL_GENERATION_MAGIC "-2\n{}", NULL,
    };
    for (int i = 0; heads[i]; i++)
        ASSERT_EQ(-1, install((const uint8_t *)heads[i], strlen(heads[i]), err));

    /* The manifest's own fields. */
    static const char *const members[] = {
        "",
        "\"format\":\"other\",\"version\":1,",
        "\"format\":\"hull-pipeline-generation\",\"version\":2,",
        "\"format\":\"hull-pipeline-generation\",\"version\":1.5,",
        "\"format\":\"hull-pipeline-generation\",",
        NULL,
    };
    for (int i = 0; members[i]; i++) {
        n = build(b, sizeof b, members[i], e, 1, 0, NULL);
        ASSERT_EQ(-1, install(b, n, err));
    }
    const char *nj = HL_GENERATION_MAGIC "8\nnot json";
    ASSERT_EQ(-1, install((const uint8_t *)nj, strlen(nj), err));
    const char *arr = HL_GENERATION_MAGIC "2\n[]";
    ASSERT_EQ(-1, install((const uint8_t *)arr, strlen(arr), err));
    ASSERT_FALSE(hl_vfs_overlay_active());
}

UTEST(generation, refuses_bad_entries)
{
    static uint8_t b[4096];
    char err[256];
    size_t n;
    /* Two entries of one name. */
    const Ent dup[] = { { "a.lua", "1", NULL, NULL },
                        { "a.lua", "2", NULL, NULL } };
    n = build(b, sizeof b, NULL, dup, 2, 0, NULL);
    ASSERT_EQ(-1, install(b, n, err));
    /* "./a.lua" is not the canonical name ("a.lua" is). */
    const Ent dot[] = { { "./a.lua", "1", NULL, NULL } };
    n = build(b, sizeof b, NULL, dot, 1, 0, NULL);
    ASSERT_EQ(-1, install(b, n, err));
    /* A position table has no schema yet. */
    const Ent tab[] = { { "a.lua", "1", NULL, "\"position_map\":\"table\"" } };
    n = build(b, sizeof b, NULL, tab, 1, 0, NULL);
    ASSERT_EQ(-1, install(b, n, err));
    ASSERT_TRUE(strstr(err, "position_map") != NULL);
    /* A size that is not a whole number. */
    const char *frac =
        "{\"format\":\"hull-pipeline-generation\",\"version\":1,\"entries\":"
        "[{\"vfs_name\":\"a\",\"content_hash\":\"6b86b273ff34fce19d6b804eff5a3f"
        "5747ada4eaa22f1d49c01e52ddb7875b4b\",\"size\":0.5}]}";
    char fb[1024];
    int fl = snprintf(fb, sizeof fb, HL_GENERATION_MAGIC "%zu\n%s1",
                      strlen(frac), frac);
    ASSERT_EQ(-1, install((const uint8_t *)fb, (size_t)fl, err));
    ASSERT_FALSE(hl_vfs_overlay_active());
}

UTEST(generation, entry_names)
{
    static const char *const ok[] = {
        "app.lua", "app.ts", "lib/m.lua", "compute/__generated/app/d_4f.wasm",
        "templates/a.html", "static/x/y.css", "migrations/001_init.sql",
        "lib/caf\xc3\xa9.lua", ".env.lua", "a/.hullx/b", "hullx.lua",
        "hull/x.lua", NULL,
    };
    for (int i = 0; ok[i]; i++)
        EXPECT_TRUE(hl_generation_name_ok(ok[i], strlen(ok[i])));
    static const char *const bad[] = {
        "", "/app.lua", "./app.lua", "a//b", "a/", "a/./b", "a/../b", "..",
        ".", "a\\b", "c:x", ".hull/x", ".HULL/x", ".hull", "hull.web.csrf",
        "hull:web:csrf", "a\x01" "b", "a\x7f", "bad\xc3", "\xff", NULL,
    };
    for (int i = 0; bad[i]; i++)
        EXPECT_FALSE(hl_generation_name_ok(bad[i], strlen(bad[i])));
    /* An embedded NUL, and a name past the cap. */
    EXPECT_FALSE(hl_generation_name_ok("a\0b", 3));
    char longn[HL_GENERATION_NAME_MAX + 2];
    memset(longn, 'a', sizeof longn - 1);
    longn[sizeof longn - 1] = '\0';
    EXPECT_FALSE(hl_generation_name_ok(longn, strlen(longn)));
    EXPECT_TRUE(hl_generation_name_ok(longn, HL_GENERATION_NAME_MAX));
}

/* A descriptor holding `len` bytes of `b`, at fd >= 3. */
static int bundle_fd(const uint8_t *b, size_t len)
{
    const char *dir = hl_test_tmpdir();
    char path[HL_TEST_PATH_MAX];
    if (!dir || snprintf(path, sizeof path, "%s/hull-gen-XXXXXX", dir)
                    >= (int)sizeof path)
        return -1;
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    if (write(fd, b, len) != (ssize_t)len || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void set_fd_env(int fd)
{
    char v[16];
    snprintf(v, sizeof v, "%d", fd);
    setenv(HL_GENERATION_ENV, v, 1);
}

UTEST(generation, take_env_reads_closes_and_installs)
{
    static uint8_t b[4096];
    const Ent e[] = { { "app.lua", "return 7\n", NULL, NULL } };
    size_t n = build(b, sizeof b, NULL, e, 1, 0, NULL);
    hl_generation_reset();
    hl_vfs_overlay_reset();

    /* Nothing handed over: nothing to do. */
    unsetenv(HL_GENERATION_ENV);
    ASSERT_EQ(0, hl_generation_take_env(0));
    ASSERT_EQ(0, hl_generation_apply(0));
    ASSERT_FALSE(hl_vfs_overlay_active());

    int fd = bundle_fd(b, n);
    ASSERT_GE(fd, 3);
    set_fd_env(fd);
    ASSERT_EQ(1, hl_generation_take_env(0));
    ASSERT_TRUE(getenv(HL_GENERATION_ENV) == NULL);  /* removed */
    ASSERT_EQ(-1, fcntl(fd, F_GETFD));               /* closed */
    /* Once per process. */
    set_fd_env(fd);
    ASSERT_EQ(0, hl_generation_take_env(0));
    unsetenv(HL_GENERATION_ENV);
    ASSERT_EQ(0, hl_generation_apply(0));
    ASSERT_TRUE(hl_vfs_overlay_find("app.lua") != NULL);
    /* Applied once; a second apply finds nothing more. */
    ASSERT_EQ(0, hl_generation_apply(0));
    hl_vfs_overlay_reset();
    hl_generation_reset();
}

UTEST(generation, take_env_refuses_no_descriptor)
{
    hl_generation_reset();
    static const char *const vals[] = { "0", "1", "2", "", "x", "3x", "-4",
                                        "99999999", NULL };
    for (int i = 0; vals[i]; i++) {
        hl_generation_reset();
        setenv(HL_GENERATION_ENV, vals[i], 1);
        EXPECT_EQ(-1, hl_generation_take_env(0));
        EXPECT_TRUE(getenv(HL_GENERATION_ENV) == NULL);
    }
    /* A descriptor number that is not open. */
    hl_generation_reset();
    setenv(HL_GENERATION_ENV, "1000", 1);
    EXPECT_EQ(-1, hl_generation_take_env(0));
    hl_generation_reset();
}

UTEST(generation, built_binary_ignores_the_descriptor)
{
    static uint8_t b[4096];
    const Ent e[] = { { "app.lua", "return 7\n", NULL, NULL } };
    size_t n = build(b, sizeof b, NULL, e, 1, 0, NULL);
    hl_generation_reset();
    hl_vfs_overlay_reset();
    int fd = bundle_fd(b, n);
    ASSERT_GE(fd, 3);
    set_fd_env(fd);
    ASSERT_EQ(0, hl_generation_take_env(1));
    ASSERT_TRUE(getenv(HL_GENERATION_ENV) == NULL);
    ASSERT_NE(-1, fcntl(fd, F_GETFD));   /* not consulted, not closed */
    char c;
    ASSERT_EQ(0, (int)lseek(fd, 0, SEEK_CUR));
    ASSERT_EQ(1, (int)read(fd, &c, 1));
    close(fd);
    ASSERT_EQ(0, hl_generation_apply(0));
    ASSERT_FALSE(hl_vfs_overlay_active());
    hl_generation_reset();
}

UTEST(generation, verify_sig_refuses_a_generation)
{
    static uint8_t b[4096];
    const Ent e[] = { { "app.lua", "return 7\n", NULL, NULL } };
    size_t n = build(b, sizeof b, NULL, e, 1, 0, NULL);
    hl_generation_reset();
    hl_vfs_overlay_reset();
    int fd = bundle_fd(b, n);
    ASSERT_GE(fd, 3);
    set_fd_env(fd);
    ASSERT_EQ(1, hl_generation_take_env(0));
    ASSERT_EQ(-1, hl_generation_apply(1));
    ASSERT_FALSE(hl_vfs_overlay_active());
    /* An invalid one is refused, never ignored. */
    b[n - 2] ^= 1;
    fd = bundle_fd(b, n);
    ASSERT_GE(fd, 3);
    hl_generation_reset();
    set_fd_env(fd);
    ASSERT_EQ(1, hl_generation_take_env(0));
    ASSERT_EQ(-1, hl_generation_apply(0));
    ASSERT_FALSE(hl_vfs_overlay_active());
    hl_generation_reset();
}

UTEST(generation, take_env_reads_a_pipe)
{
    static uint8_t b[4096];
    const Ent e[] = { { "lib/p.lua", "return 'pipe'\n", NULL, NULL } };
    size_t n = build(b, sizeof b, NULL, e, 1, 0, NULL);
    hl_generation_reset();
    hl_vfs_overlay_reset();
    int p[2];
    ASSERT_EQ(0, pipe(p));
    ASSERT_EQ((ssize_t)n, write(p[1], b, n));
    close(p[1]);
    int fd = p[0];
    if (fd < 3) { fd = fcntl(p[0], F_DUPFD, 3); close(p[0]); }
    set_fd_env(fd);
    ASSERT_EQ(1, hl_generation_take_env(0));
    ASSERT_EQ(0, hl_generation_apply(0));
    ASSERT_TRUE(hl_vfs_overlay_find("lib/p.lua") != NULL);
    hl_vfs_overlay_reset();
    hl_generation_reset();
}

UTEST(generation, verify_sig_takes_an_empty_generation)
{
    static uint8_t b[4096];
    char err[256];
    hl_vfs_overlay_reset();
    size_t n = build(b, sizeof b, NULL, NULL, 0, 0, NULL);
    /* Pipeline work that produced no app file: nothing to cover. */
    ASSERT_EQ(0, hl_generation_install_ex(b, n, 1, err, sizeof err));
    ASSERT_FALSE(hl_vfs_overlay_active());
    const Ent e[] = { { "app.lua", "return 7\n", NULL, NULL } };
    n = build(b, sizeof b, NULL, e, 1, 0, NULL);
    ASSERT_EQ(-1, hl_generation_install_ex(b, n, 1, err, sizeof err));
    ASSERT_TRUE(strstr(err, "--verify-sig") != NULL);
    ASSERT_FALSE(hl_vfs_overlay_active());
}

static void collect(const char *path, void *ud)
{
    char *out = ud;
    strcat(out, path);
    strcat(out, ";");
}

UTEST(generation, inputs_are_read_from_the_manifest)
{
    static uint8_t b[4096];
    char got[256] = "";
    const char *m = "{\"format\":\"hull-pipeline-generation\",\"version\":1,"
                    "\"entries\":[],\"inputs\":[\".\",\"src/a.lua\"]}";
    int n = snprintf((char *)b, sizeof b, HL_GENERATION_MAGIC "%zu\n%s",
                     strlen(m), m);
    ASSERT_EQ(0, hl_generation_inputs(b, (size_t)n, collect, got));
    ASSERT_STREQ(".;src/a.lua;", got);
    ASSERT_EQ(-1, hl_generation_inputs((const uint8_t *)"nope", 4, collect, got));
}

UTEST(generation, a_pipeline_needs_a_build_config)
{
    const char *dir = hl_test_tmpdir();
    ASSERT_TRUE(dir != NULL);
    char app[HL_TEST_PATH_MAX], cfg[HL_TEST_PATH_MAX + 32];
    snprintf(app, sizeof app, "%s/hull-gen-app-XXXXXX", dir);
    ASSERT_TRUE(mkdtemp(app) != NULL);
    ASSERT_FALSE(hl_generation_has_pipeline(app));
    ASSERT_FALSE(hl_generation_has_pipeline(NULL));
    /* No config: hl_generation_take runs nothing. */
    hl_generation_reset();
    unsetenv(HL_GENERATION_ENV);
    ASSERT_EQ(0, hl_generation_take(0, app));
    ASSERT_EQ(0, hl_generation_apply(0));
    snprintf(cfg, sizeof cfg, "%s/hull.build.json", app);
    /* A link is not a config. */
    ASSERT_EQ(0, symlink("/dev/null", cfg));
    ASSERT_FALSE(hl_generation_has_pipeline(app));
    unlink(cfg);
    FILE *f = fopen(cfg, "w");
    ASSERT_TRUE(f != NULL);
    fputs("{}\n", f);
    fclose(f);
    ASSERT_TRUE(hl_generation_has_pipeline(app));
    /* A built binary runs no pipeline, whatever the tree holds. */
    hl_generation_reset();
    ASSERT_EQ(0, hl_generation_take(1, app));
    unlink(cfg);
    rmdir(app);
    hl_generation_reset();
}

/* Write an empty file <app>/<name>. */
static int touch_in(const char *app, const char *name)
{
    char p[HL_TEST_PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/%s", app, name);
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    fclose(f);
    return 0;
}

static void unlink_in(const char *app, const char *name)
{
    char p[HL_TEST_PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/%s", app, name);
    unlink(p);
}

UTEST(generation, a_typescript_entry_runs_the_pipeline)
{
    const char *dir = hl_test_tmpdir();
    ASSERT_TRUE(dir != NULL);
    char app[HL_TEST_PATH_MAX], buf[HL_TEST_PATH_MAX + 32], want[HL_TEST_PATH_MAX + 32];
    char err[256];
    snprintf(app, sizeof app, "%s/hull-gen-ts-XXXXXX", dir);
    ASSERT_TRUE(mkdtemp(app) != NULL);

    ASSERT_EQ(0, hl_app_entry_ts(app, buf, sizeof buf, err, sizeof err));
    ASSERT_FALSE(hl_generation_has_pipeline(app));

    ASSERT_EQ(0, touch_in(app, "app.ts"));
    ASSERT_EQ(1, hl_app_entry_ts(app, buf, sizeof buf, err, sizeof err));
    snprintf(want, sizeof want, "%s/app.ts", app);
    ASSERT_STREQ(want, buf);
    ASSERT_TRUE(hl_generation_has_pipeline(app));

    /* Beside another entry it is ambiguous, and the message names both. */
    ASSERT_EQ(0, touch_in(app, "app.js"));
    ASSERT_EQ(-1, hl_app_entry_ts(app, buf, sizeof buf, err, sizeof err));
    ASSERT_TRUE(strstr(err, "app.ts") && strstr(err, "app.js"));
    unlink_in(app, "app.js");
    ASSERT_EQ(0, touch_in(app, "app.lua"));
    ASSERT_EQ(-1, hl_app_entry_ts(app, buf, sizeof buf, err, sizeof err));
    unlink_in(app, "app.lua");
    ASSERT_EQ(0, touch_in(app, "app.mts"));
    ASSERT_EQ(-1, hl_app_entry_ts(app, buf, sizeof buf, err, sizeof err));
    ASSERT_TRUE(strstr(err, "app.mts") != NULL);
    unlink_in(app, "app.ts");

    ASSERT_EQ(1, hl_app_entry_ts(app, buf, sizeof buf, NULL, 0));
    snprintf(want, sizeof want, "%s/app.mts", app);
    ASSERT_STREQ(want, buf);
    ASSERT_TRUE(hl_generation_has_pipeline(app));
    unlink_in(app, "app.mts");
    rmdir(app);

    ASSERT_TRUE(hl_app_entry_is_ts("lib/a.ts"));
    ASSERT_TRUE(hl_app_entry_is_ts("app.mts"));
    ASSERT_FALSE(hl_app_entry_is_ts("a.cts"));
    ASSERT_FALSE(hl_app_entry_is_ts(".ts"));
    ASSERT_FALSE(hl_app_entry_is_ts("a.js"));
    ASSERT_TRUE(hl_app_name_is_ts("a.cts"));
    ASSERT_TRUE(hl_app_name_is_ts("a.tsx"));
    ASSERT_FALSE(hl_app_name_is_ts("a.mjs"));
    ASSERT_FALSE(hl_app_name_is_ts(NULL));
}

UTEST_MAIN()
