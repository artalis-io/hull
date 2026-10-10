/*
 * test_runtime_flags.c - the --hull-<name> rewrite and the built-binary
 * downgrade rule (include/hull/runtime_flags.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/runtime_flags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A long --hull-ca-bundle=PATH used to overflow the 96-byte rewrite pool:
 * the function returned 0 and left "--hull-..." in place, which a built
 * binary then took as the start of the app's arguments. */
UTEST(runtime_flags, long_value_is_rewritten_not_dropped)
{
    char arg[700];
    char path[600];
    memset(path, 'a', sizeof path - 1);
    path[0] = '/';
    path[sizeof path - 1] = '\0';
    snprintf(arg, sizeof arg, "--hull-ca-bundle=%s", path);
    char *a = arg;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), 1);
    ASSERT_EQ(strncmp(a, "--ca-bundle=", 12), 0);
    ASSERT_STREQ(a + 12, path);
}

/* The pool held 32 rewrites; the 33rd was silently left prefixed. */
UTEST(runtime_flags, many_rewrites_all_taken)
{
    for (int i = 0; i < 100; i++) {
        char buf[] = "--hull-no-sandbox";
        char *a = buf;
        ASSERT_EQ(hl_runtime_flag_unprefix(&a), 1);
        ASSERT_STREQ(a, "--no-sandbox");
    }
}

UTEST(runtime_flags, one_letter_names)
{
    char buf[] = "--hull-d";
    char *a = buf;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), 1);
    ASSERT_STREQ(a, "-d");

    /* There is no "-d=x" form: refuse rather than produce an option that
     * matches nothing (and so became an app argument). */
    char buf2[] = "--hull-d=x.db";
    a = buf2;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), -1);
}

UTEST(runtime_flags, not_prefixed)
{
    char b1[] = "--no-sandbox";
    char *a = b1;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), 0);
    ASSERT_STREQ(a, "--no-sandbox");
    char b2[] = "--hull-";
    a = b2;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), 0);
    char b3[] = "--hull-=x";
    a = b3;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), -1);
}

UTEST(runtime_flags, downgrades_need_the_prefix_in_a_built_binary)
{
    static const char *const down[] = {
        "--no-sandbox", "--ca-bundle=/x", "--max-instructions", "--max-run-ms",
        "-b", "-d", "-m", "-M", "-s", "--tls-cert", "--tls-key",
        "--wasm-gas", "--wasm-heap", "--wasm-stack", "--wasm-max-input",
        "--wasm-max-output", "--body-max-size",
        /* Raise the WASM wall-clock bound (to 1 h) / the connection cap. */
        "--wasm-timeout-ms", "--wasm-timeout-ms=3600000",
        "--max-connections",
        /* Round 7: a 10-minute read timeout is the slowloris lever; the
         * worker / queue / drain sizes are resource limits too; --agent
         * writes sidecars into the app directory. */
        "--read-timeout", "--read-timeout=600000", "--workers",
        "--queue-capacity", "--drain-timeout", "--agent", "--agent-api",
        NULL
    };
    for (int i = 0; down[i]; i++) {
        EXPECT_EQ(hl_runtime_flag_is_downgrade(down[i]), 1);
        EXPECT_EQ(hl_runtime_flag_check(down[i], 0, 1, HL_RF_SERVE), -1); /* bare, built */
        EXPECT_EQ(hl_runtime_flag_check(down[i], 1, 1, HL_RF_SERVE), 0);  /* prefixed */
        EXPECT_EQ(hl_runtime_flag_check(down[i], 0, 0, HL_RF_SERVE), 0);  /* under hull */
    }
    static const char *const ok[] = {
        "-p", "-l", "--no-migrate", "--verify-sig", "--audit", "-dx",
        "--bogus", "--help", "--no-db", "--gpu-device", "--agentx", NULL
    };
    for (int i = 0; ok[i]; i++) {
        EXPECT_EQ(hl_runtime_flag_is_downgrade(ok[i]), 0);
        EXPECT_EQ(hl_runtime_flag_check(ok[i], 0, 1, HL_RF_SERVE), 0);
    }
}

/* The app.main runner (serve_cli.c) implements only some downgrade options.
 * The rest are a built CLI tool's own arguments: refusing `./tool -s pat`
 * protected nothing (the runner has no -s) and left no spelling that reached
 * the app. The ones it does implement stay reserved - including the --wasm-*
 * ceilings, which it parses since round 6 and used to honour bare (round 7
 * M2: `./tool --wasm-timeout-ms 3600000` lifted the signed manifest's
 * ceilings to the compile-time maxima). */
UTEST(runtime_flags, cli_runner_reserves_only_what_it_implements)
{
    static const char *const app_owned[] = {
        "-s", "-m", "-M", "-b", "-l", "-p", "--tls-cert", "--tls-key",
        "--body-max-size", "--max-connections", "--agent", "--agent-api",
        "--read-timeout", "--workers", "--queue-capacity", "--drain-timeout",
        NULL
    };
    for (int i = 0; app_owned[i]; i++) {
        EXPECT_EQ(hl_runtime_flag_takes(app_owned[i], HL_RF_CLI), 0);
        EXPECT_EQ(hl_runtime_flag_check(app_owned[i], 0, 1, HL_RF_CLI), 0);
        /* serve.c implements them all, so there they stay reserved. */
        if (hl_runtime_flag_is_downgrade(app_owned[i]))
            EXPECT_EQ(hl_runtime_flag_check(app_owned[i], 0, 1, HL_RF_SERVE), -1);
    }
    static const char *const reserved[] = {
        "--no-sandbox", "--allow-degraded-sandbox", "--no-ca-bundle",
        "--skip-ca-bundle", "--ca-bundle", "--ca-bundle=/x",
        "--no-verify-platform", "--max-instructions", "--max-run-ms", "-d",
#ifdef HL_ENABLE_WASM
        "--wasm-heap", "--wasm-stack", "--wasm-gas", "--wasm-timeout-ms",
        "--wasm-timeout-ms=3600000", "--wasm-max-input", "--wasm-max-output",
#endif
        NULL
    };
    for (int i = 0; reserved[i]; i++) {
        EXPECT_EQ(hl_runtime_flag_takes(reserved[i], HL_RF_CLI), 1);
        EXPECT_EQ(hl_runtime_flag_check(reserved[i], 0, 1, HL_RF_CLI), -1);
        EXPECT_EQ(hl_runtime_flag_check(reserved[i], 1, 1, HL_RF_CLI), 0);
        EXPECT_EQ(hl_runtime_flag_is_downgrade(reserved[i]), 1);
    }
}

/* The reservation is derived from the table, row by row: an option a runner
 * takes is refused bare in a built binary exactly when its row says it is a
 * downgrade, and an option a runner does not take is always left alone. */
UTEST(runtime_flags, reservation_follows_the_table)
{
    static const unsigned runners[] = { HL_RF_SERVE, HL_RF_CLI };
    int rows = 0;
    for (const HlRuntimeFlag *f = hl_runtime_flags(); f->name; f++, rows++) {
        EXPECT_TRUE(f->flags & (HL_RF_SERVE | HL_RF_CLI | HL_RF_DOWNGRADE));
        for (size_t r = 0; r < sizeof runners / sizeof runners[0]; r++) {
            int want = ((f->flags & runners[r]) && (f->flags & HL_RF_DOWNGRADE)) ? -1 : 0;
            EXPECT_EQ(hl_runtime_flag_check(f->name, 0, 1, runners[r]), want);
            EXPECT_EQ(hl_runtime_flag_takes(f->name, runners[r]),
                      (f->flags & runners[r]) ? 1 : 0);
        }
    }
    EXPECT_GT(rows, 30);
}

/* Read one function's body out of a source file (from @p sig to the first
 * line that is exactly "}"). Run from the repo root, as `make test` does. */
static char *read_function(const char *path, const char *sig)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    char *b = strstr(buf, sig);
    if (!b) return NULL;
    char *e = strstr(b, "\n}\n");
    if (!e) return NULL;
    size_t len = (size_t)(e - b);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, b, len);
    out[len] = '\0';
    return out;
}

/* Every option literal @p body compares an argument against (`argv[i], "x"`
 * or `a, "x"`) must be a row @p runner takes. Returns how many it checked,
 * or -1 when one is missing (named on stderr). */
static int check_parsed_names(const char *body, const char *needle,
                              unsigned runner, const char *where)
{
    int n = 0, missing = 0;
    size_t nl = strlen(needle);
    for (const char *p = strstr(body, needle); p; p = strstr(p + 1, needle)) {
        const char *q = p + nl;
        const char *e = strchr(q, '"');
        if (!e) break;
        char name[64];
        size_t len = (size_t)(e - q);
        if (len == 0 || len >= sizeof name) continue;
        memcpy(name, q, len);
        name[len] = '\0';
        if (name[len - 1] == '=') name[len - 1] = '\0';   /* "--ca-bundle=" */
        if (strcmp(name, "--") == 0) continue;
        if (!hl_runtime_flag_takes(name, runner)) {
            fprintf(stderr, "%s parses %s, which hl_runtime_flags() does not "
                    "give it\n", where, name);
            missing = 1;
        }
        n++;
    }
    return missing ? -1 : n;
}

/* The drift guard. A parser branch for an option the table does not give its
 * runner is unreachable (each parser asks hl_runtime_flag_takes first), so
 * an option added to a parser without a table row would silently do nothing;
 * this catches it at test time instead, from the parser's own source. */
UTEST(runtime_flags, every_parsed_option_is_in_the_table)
{
    char *serve = read_function("src/hull/serve.c",
                                "static int hl_parse_serve_args(");
    char *cli = read_function("src/hull/serve_cli.c",
                              "static int cli_parse_args(");
    char *wasm = read_function("include/hull/wasm_config.h",
                               "static inline int hl_wasm_cli_flag(");
    ASSERT_TRUE(serve != NULL);
    ASSERT_TRUE(cli != NULL);
    ASSERT_TRUE(wasm != NULL);
    /* Both parsers gate on the table before any branch. */
    EXPECT_TRUE(strstr(serve, "hl_runtime_flag_takes(argv[i], HL_RF_SERVE)") != NULL);
    EXPECT_TRUE(strstr(cli, "hl_runtime_flag_takes(argv[i], HL_RF_CLI)") != NULL);

    EXPECT_GT(check_parsed_names(serve, "argv[i], \"",
                                 HL_RF_SERVE, "serve.c"), 30);
    EXPECT_GT(check_parsed_names(cli, "argv[i], \"",
                                 HL_RF_CLI, "serve_cli.c"), 10);
    /* hl_wasm_cli_flag is the CLI runner's --wasm-* parser (serve.c spells
     * its own); the CLI rows for them exist only with WASM compiled in. */
    EXPECT_EQ(check_parsed_names(wasm, "strcmp(a, \"",
                                 HL_RF_SERVE, "hl_wasm_cli_flag"), 6);
#ifdef HL_ENABLE_WASM
    EXPECT_EQ(check_parsed_names(wasm, "strcmp(a, \"",
                                 HL_RF_CLI, "hl_wasm_cli_flag"), 6);
#endif
    free(serve);
    free(cli);
    free(wasm);
}

UTEST(runtime_flags, unknown_prefixed_is_an_error)
{
    EXPECT_EQ(hl_runtime_flag_unknown("--bogus"), -1);
    EXPECT_EQ(hl_runtime_flag_unknown("-q"), -1);
}

UTEST_MAIN();
