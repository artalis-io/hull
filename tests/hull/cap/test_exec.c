/*
 * test_exec.c - the declared-command policy and the argument matcher
 * (cap/exec.c, docs/design/exec_broker.md §3 / §4).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/exec.h"

#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

#define S(x) x, sizeof(x) - 1

/* unit-state from the design doc: fixed args, `--`, then a repeat slot. */
static int build_unit_state(HlExecBuf *b, int64_t rmin, int64_t rmax)
{
    hl_exec_b_init(b, NULL);
    hl_exec_b_cmd(b, S("unit-state"), S("/usr/bin/systemctl"), -1, 64, -1, -1, 1, NULL, 0);
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);   /* env */
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);   /* fs.read */
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);   /* fs.write */
    size_t m = hl_exec_b_count_begin(b);
    hl_exec_b_fixed(b, S("show"));
    hl_exec_b_fixed(b, S("--no-pager"));
    hl_exec_b_fixed(b, S("--"));
    hl_exec_b_slot(b, 0, -1, 64, 1, rmin, rmax, S("hs-"), S(".service"),
                   S("a-z0-9-"));
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);   /* enum */
    hl_exec_b_count_end(b, m, 4);
    return hl_exec_b_finish(b);
}

/* One command with a single slot, built from the given slot parameters,
 * and its enum values (NULL-terminated). */
static int build_one_slot(HlExecBuf *b, int dash, int64_t min, int64_t max,
                          const char *prefix, const char *suffix,
                          const char *chars, const char *const *enums)
{
    hl_exec_b_init(b, NULL);
    hl_exec_b_cmd(b, S("cmd"), S("/usr/bin/tool"), -1, -1, -1, -1, 0, NULL, 0);
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
    size_t m = hl_exec_b_count_begin(b);
    hl_exec_b_slot(b, dash, min, max, 0, -1, -1,
                   prefix, prefix ? strlen(prefix) : 0,
                   suffix, suffix ? strlen(suffix) : 0,
                   chars, chars ? strlen(chars) : 0);
    size_t e = hl_exec_b_count_begin(b);
    size_t ne = 0;
    for (; enums && enums[ne]; ne++) hl_exec_b_str(b, enums[ne], strlen(enums[ne]));
    hl_exec_b_count_end(b, e, ne);
    hl_exec_b_count_end(b, m, 1);
    return hl_exec_b_finish(b);
}

static int call1(const HlExecBuf *b, const char *v, size_t n)
{
    const char *a[1] = { v };
    size_t l[1] = { n };
    return hl_exec_check_call(b->data, b->len, S("cmd"), a, l, 1, NULL, NULL, 0);
}

static int calln(const HlExecBuf *b, const char *const *a, size_t n, char *err)
{
    size_t l[HL_EXEC_MAX_CALL_ARGS];
    for (size_t i = 0; i < n; i++) l[i] = strlen(a[i]);
    return hl_exec_check_call(b->data, b->len, S("unit-state"), a, l, n, NULL,
                              err, 256);
}

/* A command whose rest is given raw, to reach the validator's refusals. */
typedef struct {
    const char *name, *path;
    int64_t timeout, so, se, conc;
    const char *env_k, *env_v;
    const char *fs_write;
    int repeat_first;   /* a repeat slot followed by a fixed arg */
} RawCmd;

static int build_raw(HlExecBuf *b, const RawCmd *c)
{
    hl_exec_b_init(b, NULL);
    hl_exec_b_cmd(b, c->name, strlen(c->name), c->path, strlen(c->path),
                  c->timeout, c->so, c->se, c->conc, 0, NULL, 0);
    size_t m = hl_exec_b_count_begin(b);
    if (c->env_k) {
        hl_exec_b_str(b, c->env_k, strlen(c->env_k));
        hl_exec_b_str(b, c->env_v, strlen(c->env_v));
    }
    hl_exec_b_count_end(b, m, c->env_k ? 1 : 0);
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
    m = hl_exec_b_count_begin(b);
    if (c->fs_write) hl_exec_b_str(b, c->fs_write, strlen(c->fs_write));
    hl_exec_b_count_end(b, m, c->fs_write ? 1 : 0);
    m = hl_exec_b_count_begin(b);
    size_t n = 0;
    if (c->repeat_first) {
        hl_exec_b_slot(b, 0, -1, -1, 1, 1, 2, NULL, 0, NULL, 0, NULL, 0);
        hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
        hl_exec_b_fixed(b, S("x"));
        n = 2;
    }
    hl_exec_b_count_end(b, m, n);
    return hl_exec_b_finish(b);
}

static RawCmd ok_cmd(void)
{
    RawCmd c = { "cmd", "/usr/bin/tool", -1, -1, -1, -1, NULL, NULL, NULL, 0 };
    return c;
}

/* ── Round trip ────────────────────────────────────────────────────── */

UTEST(exec_policy, builds_validates_and_reads_back)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_unit_state(&b, -1, 32));
    ASSERT_EQ(0, hl_exec_policy_validate(b.data, b.len, NULL, 0));
    ASSERT_EQ(1, hl_exec_policy_count(b.data, b.len));

    HlExecCmd c;
    ASSERT_EQ(0, hl_exec_policy_find(b.data, b.len, S("unit-state"), &c));
    EXPECT_STREQ("/usr/bin/systemctl", c.path);
    EXPECT_EQ((uint32_t)HL_EXEC_TIMEOUT_DEFAULT_MS, c.timeout_ms);
    EXPECT_EQ(64u, c.stdout_max);
    EXPECT_EQ((uint32_t)HL_EXEC_STDERR_DEFAULT, c.stderr_max);
    EXPECT_EQ(HL_EXEC_CONCURRENCY_DEFAULT, c.concurrency);
    EXPECT_TRUE(c.flags & HL_EXEC_CMD_UNIX_SOCKETS);
    EXPECT_EQ(4, c.n_argv);

    const uint8_t *cur = c.argv;
    HlExecArg a;
    hl_exec_next_arg(&cur, c.end, &a);
    EXPECT_FALSE(a.is_slot);
    EXPECT_STREQ("show", a.fixed);
    hl_exec_next_arg(&cur, c.end, &a);
    hl_exec_next_arg(&cur, c.end, &a);
    EXPECT_STREQ("--", a.fixed);
    hl_exec_next_arg(&cur, c.end, &a);
    ASSERT_TRUE(a.is_slot);
    EXPECT_TRUE(a.slot.flags & HL_EXEC_SLOT_REPEAT);
    EXPECT_EQ(1, a.slot.repeat_min);   /* the default is 1 */
    EXPECT_EQ(32, a.slot.repeat_max);
    EXPECT_EQ(0, hl_exec_policy_find(b.data, b.len, S("unit-stat"), &c) == 0);
    hl_exec_buf_free(&b);
}

UTEST(exec_policy, an_empty_section_is_a_policy_of_no_commands)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    ASSERT_EQ(0, hl_exec_b_finish(&b));
    EXPECT_EQ(0, hl_exec_policy_count(b.data, b.len));
    EXPECT_EQ(HL_EXEC_E_UNKNOWN,
              hl_exec_check_call(b.data, b.len, S("x"), NULL, NULL, 0, NULL, NULL, 0));
    hl_exec_buf_free(&b);
}

/* ── The call: arguments against slots ─────────────────────────────── */

UTEST(exec_call, unit_state_takes_one_to_32_units)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_unit_state(&b, 1, 32));
    char err[256];
    const char *one[] = { "hs-api.service" };
    EXPECT_EQ(HL_EXEC_OK, calln(&b, one, 1, err));
    const char *two[] = { "hs-api.service", "hs-db.service" };
    EXPECT_EQ(HL_EXEC_OK, calln(&b, two, 2, err));
    /* none: systemctl show would query the manager itself */
    EXPECT_EQ(HL_EXEC_E_ARGS, calln(&b, NULL, 0, err));
    EXPECT_TRUE(strstr(err, "takes 1..32 arguments") != NULL);
    const char *many[33];
    for (int i = 0; i < 33; i++) many[i] = "hs-a.service";
    EXPECT_EQ(HL_EXEC_E_ARGS, calln(&b, many, 33, err));
    hl_exec_buf_free(&b);
}

UTEST(exec_call, option_injection_is_refused)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_unit_state(&b, 1, 32));
    char err[256];
    const char *a[] = { "--host=evil" };
    EXPECT_EQ(HL_EXEC_E_ARGS, calln(&b, a, 1, err));
    EXPECT_TRUE(strstr(err, "starts with '-'") != NULL);
    const char *c[] = { "hs-api.service", "-H" };
    EXPECT_EQ(HL_EXEC_E_ARGS, calln(&b, c, 2, err));
    EXPECT_TRUE(strstr(err, "argument 2") != NULL);
    hl_exec_buf_free(&b);
}

UTEST(exec_call, prefix_suffix_and_class)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_unit_state(&b, 1, 32));
    char err[256];
    const char *bad_prefix[] = { "sshd.service" };
    EXPECT_EQ(HL_EXEC_E_ARGS, calln(&b, bad_prefix, 1, err));
    const char *bad_suffix[] = { "hs-api.socket" };
    EXPECT_EQ(HL_EXEC_E_ARGS, calln(&b, bad_suffix, 1, err));
    const char *bad_char[] = { "hs-API.service" };
    EXPECT_EQ(HL_EXEC_E_ARGS, calln(&b, bad_char, 1, err));
    const char *dot[] = { "hs-a.b.service" };   /* '.' not in a-z0-9- */
    EXPECT_EQ(HL_EXEC_E_ARGS, calln(&b, dot, 1, err));
    const char *slash[] = { "hs-../x.service" };
    EXPECT_EQ(HL_EXEC_E_ARGS, calln(&b, slash, 1, err));
    hl_exec_buf_free(&b);
}

UTEST(exec_call, the_unknown_name_is_its_own_error)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_unit_state(&b, 1, 32));
    EXPECT_EQ(HL_EXEC_E_UNKNOWN,
              hl_exec_check_call(b.data, b.len, S("gpu-query"), NULL, NULL, 0,
                                 NULL, NULL, 0));
    /* a NULL policy (no section) answers the same */
    EXPECT_EQ(HL_EXEC_E_UNKNOWN,
              hl_exec_check_call(NULL, 0, S("unit-state"), NULL, NULL, 0,
                                 NULL, NULL, 0));
    hl_exec_buf_free(&b);
}

UTEST(exec_call, arguments_are_capped_together)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_one_slot(&b, 0, -1, 4096, NULL, NULL, NULL, NULL));
    char *big = malloc(HL_EXEC_MAX_ARG_BYTES + 1);
    ASSERT_TRUE(big != NULL);
    memset(big, 'a', HL_EXEC_MAX_ARG_BYTES + 1);
    const char *a[1] = { big };
    size_t l[1] = { HL_EXEC_MAX_ARG_BYTES + 1 };
    char err[256];
    EXPECT_EQ(HL_EXEC_E_ARGS,
              hl_exec_check_call(b.data, b.len, S("cmd"), a, l, 1, NULL, err, sizeof err));
    EXPECT_TRUE(strstr(err, "longer than") != NULL);
    free(big);
    hl_exec_buf_free(&b);
}

/* ── The slot matcher ──────────────────────────────────────────────── */

UTEST(exec_slot, default_class_and_lengths)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_one_slot(&b, 0, 2, 5, NULL, NULL, NULL, NULL));
    EXPECT_EQ(HL_EXEC_OK, call1(&b, S("a.b_c")));
    EXPECT_EQ(HL_EXEC_OK, call1(&b, S("A9")));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("a")));        /* < min */
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("abcdef")));   /* > max */
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("a b")));      /* space */
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("a/b")));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("a=b")));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, "a\0b", 3));     /* NUL */
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, "", 0));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, "\xc3\xa9t", 3)); /* not ASCII */
    hl_exec_buf_free(&b);
}

UTEST(exec_slot, leading_dash_needs_allow_dash)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_one_slot(&b, 0, -1, -1, NULL, NULL, NULL, NULL));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("-x")));
    EXPECT_EQ(HL_EXEC_OK, call1(&b, S("x-")));
    hl_exec_buf_free(&b);
    ASSERT_EQ(0, build_one_slot(&b, 1, -1, -1, NULL, NULL, NULL, NULL));
    EXPECT_EQ(HL_EXEC_OK, call1(&b, S("-x")));
    hl_exec_buf_free(&b);
}

UTEST(exec_slot, custom_class)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_one_slot(&b, 0, -1, -1, NULL, NULL, "0-9,", NULL));
    EXPECT_EQ(HL_EXEC_OK, call1(&b, S("0,1,2")));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("0;1")));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("a")));
    hl_exec_buf_free(&b);
    /* '-' is a literal at the end */
    ASSERT_EQ(0, build_one_slot(&b, 0, -1, -1, NULL, NULL, "a-c-", NULL));
    EXPECT_EQ(HL_EXEC_OK, call1(&b, S("a-b")));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("d")));
    hl_exec_buf_free(&b);
}

UTEST(exec_slot, bad_class_specs_fail_the_build)
{
    HlExecBuf b;
    EXPECT_EQ(-1, build_one_slot(&b, 0, -1, -1, NULL, NULL, "z-a", NULL));
    EXPECT_TRUE(strstr(b.err, "backwards") != NULL);
    EXPECT_EQ(-1, build_one_slot(&b, 0, -1, -1, NULL, NULL, "a b", NULL));
    EXPECT_EQ(-1, build_one_slot(&b, 0, -1, -1, NULL, NULL, "", NULL));
    EXPECT_EQ(-1, build_one_slot(&b, 0, -1, -1, NULL, NULL, "\x01", NULL));
    EXPECT_TRUE(b.data == NULL);   /* freed on failure */
}

UTEST(exec_slot, enum_values)
{
    HlExecBuf b;
    const char *const e[] = { "active", "failed", NULL };
    ASSERT_EQ(0, build_one_slot(&b, 0, -1, -1, NULL, NULL, NULL, e));
    EXPECT_EQ(HL_EXEC_OK, call1(&b, S("active")));
    EXPECT_EQ(HL_EXEC_OK, call1(&b, S("failed")));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("activ")));
    EXPECT_EQ(HL_EXEC_E_ARGS, call1(&b, S("actives")));
    hl_exec_buf_free(&b);
}

/* ── Validation refusals ───────────────────────────────────────────── */

UTEST(exec_validate, command_fields)
{
    HlExecBuf b;
    RawCmd c;

    c = ok_cmd();
    EXPECT_EQ(0, build_raw(&b, &c));
    hl_exec_buf_free(&b);

    c = ok_cmd(); c.name = "Bad";
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.name = "-x";
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.path = "usr/bin/tool";
    EXPECT_EQ(-1, build_raw(&b, &c));
    EXPECT_TRUE(strstr(b.err, "absolute") != NULL);
    c = ok_cmd(); c.path = "/usr/../bin/sh";
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.path = "/usr//bin/tool";
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.path = "/usr/bin/";
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.timeout = 99;
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.timeout = 60001;
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.timeout = -2;          /* a negative value as written */
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.so = (int64_t)HL_EXEC_STDOUT_CEIL + 1;
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.se = (int64_t)HL_EXEC_STDERR_CEIL + 1;
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.conc = 0;
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.conc = HL_EXEC_CONCURRENCY_MAX + 1;
    EXPECT_EQ(-1, build_raw(&b, &c));
    c = ok_cmd(); c.so = 0;                /* discarding stdout is allowed */
    EXPECT_EQ(0, build_raw(&b, &c));
    hl_exec_buf_free(&b);
}

UTEST(exec_validate, env_names)
{
    HlExecBuf b;
    RawCmd c = ok_cmd();
    c.env_k = "LANG"; c.env_v = "C.UTF-8";
    EXPECT_EQ(0, build_raw(&b, &c));
    hl_exec_buf_free(&b);
    const char *const refused[] = { "LD_PRELOAD", "LD_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES",
                                    "PATH", "IFS", "ENV", "BASH_ENV", "1X", "A-B", "", NULL };
    for (int i = 0; refused[i]; i++) {
        c = ok_cmd();
        c.env_k = refused[i]; c.env_v = "x";
        EXPECT_EQ(-1, build_raw(&b, &c));
    }
}

UTEST(exec_validate, fs_write_only_under_dev_run_tmp)
{
    HlExecBuf b;
    RawCmd c = ok_cmd();
    c.fs_write = "/dev/nvidiactl";
    EXPECT_EQ(0, build_raw(&b, &c));
    hl_exec_buf_free(&b);
    c.fs_write = "/run/hs/x";
    EXPECT_EQ(0, build_raw(&b, &c));
    hl_exec_buf_free(&b);
    c.fs_write = "/etc/passwd";
    EXPECT_EQ(-1, build_raw(&b, &c));
    c.fs_write = "/tmp";                    /* the directory itself */
    EXPECT_EQ(-1, build_raw(&b, &c));
    c.fs_write = "/devx/y";
    EXPECT_EQ(-1, build_raw(&b, &c));
    c.fs_write = "/dev/../etc/shadow";
    EXPECT_EQ(-1, build_raw(&b, &c));
}

UTEST(exec_validate, only_the_last_entry_repeats)
{
    HlExecBuf b;
    RawCmd c = ok_cmd();
    c.repeat_first = 1;
    EXPECT_EQ(-1, build_raw(&b, &c));
    EXPECT_TRUE(strstr(b.err, "only the last argv entry may repeat") != NULL);
}

UTEST(exec_validate, slot_rules)
{
    HlExecBuf b;
    const char *const dash_enum[] = { "-v", NULL };
    EXPECT_EQ(-1, build_one_slot(&b, 0, -1, -1, NULL, NULL, NULL, dash_enum));
    EXPECT_EQ(0, build_one_slot(&b, 1, -1, -1, NULL, NULL, NULL, dash_enum));
    hl_exec_buf_free(&b);
    const char *const e[] = { "a", NULL };
    EXPECT_EQ(-1, build_one_slot(&b, 0, -1, -1, "p", NULL, NULL, e));   /* enum + prefix */
    EXPECT_EQ(-1, build_one_slot(&b, 0, 0, -1, NULL, NULL, NULL, NULL));   /* min 0 */
    EXPECT_EQ(-1, build_one_slot(&b, 0, 5, 4, NULL, NULL, NULL, NULL));    /* min > max */
    EXPECT_EQ(-1, build_one_slot(&b, 0, -1, HL_EXEC_SLOT_LEN_CEIL + 1, NULL, NULL, NULL, NULL));
    EXPECT_EQ(-1, build_one_slot(&b, 0, -1, 4, "abc", "de", NULL, NULL));  /* affixes > max */
    EXPECT_EQ(-1, build_one_slot(&b, 0, -1, -1, "-x", NULL, NULL, NULL));  /* dash prefix */
}

UTEST(exec_validate, too_many_and_duplicate_commands)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    for (int i = 0; i < HL_EXEC_MAX_COMMANDS + 1; i++) {
        char name[8];
        snprintf(name, sizeof name, "c%d", i);
        hl_exec_b_cmd(&b, name, strlen(name), S("/bin/true"), -1, -1, -1, -1, 0, NULL, 0);
        for (int k = 0; k < 4; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    }
    EXPECT_EQ(-1, hl_exec_b_finish(&b));
    hl_exec_b_init(&b, NULL);
    for (int i = 0; i < 2; i++) {
        hl_exec_b_cmd(&b, S("same"), S("/bin/true"), -1, -1, -1, -1, 0, NULL, 0);
        for (int k = 0; k < 4; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    }
    EXPECT_EQ(-1, hl_exec_b_finish(&b));
    EXPECT_TRUE(strstr(b.err, "twice") != NULL);
}

/* ── Untrusted bytes ───────────────────────────────────────────────── */

UTEST(exec_validate, every_truncation_is_refused)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_unit_state(&b, 1, 32));
    for (size_t n = 0; n < b.len; n++)
        EXPECT_EQ(-1, hl_exec_policy_validate(b.data, n, NULL, 0));
    /* one byte too many */
    uint8_t *x = malloc(b.len + 1);
    ASSERT_TRUE(x != NULL);
    memcpy(x, b.data, b.len);
    x[b.len] = 0;
    EXPECT_EQ(-1, hl_exec_policy_validate(x, b.len + 1, NULL, 0));
    free(x);
    hl_exec_buf_free(&b);
}

UTEST(exec_validate, every_byte_flip_is_refused_or_still_well_formed)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_unit_state(&b, 1, 32));
    uint8_t *x = malloc(b.len);
    ASSERT_TRUE(x != NULL);
    for (size_t i = 0; i < b.len; i++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(x, b.data, b.len);
            x[i] ^= (uint8_t)(1u << bit);
            if (hl_exec_policy_validate(x, b.len, NULL, 0) != 0) continue;
            /* Accepted: then every reader must stay inside it. */
            int n = hl_exec_policy_count(x, b.len);
            for (int k = 0; k < n; k++) {
                HlExecCmd c;
                EXPECT_EQ(0, hl_exec_policy_at(x, b.len, k, &c));
                const char *a[1] = { "hs-a.service" };
                size_t l[1] = { 12 };
                (void)hl_exec_check_call(x, b.len, c.name, strlen(c.name), a, l, 1,
                                         NULL, NULL, 0);
            }
        }
    }
    free(x);
    hl_exec_buf_free(&b);
}

UTEST(exec_validate, a_chars_bitmap_with_a_control_byte_is_refused)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_one_slot(&b, 0, -1, -1, NULL, NULL, "a", NULL));
    /* The bitmap is the 32 bytes before the enum count (u16, last 4 bytes
     * of the policy hold the enum count and nothing else follows). */
    uint8_t *bm = b.data + b.len - 2 - 32;
    bm[0] |= 1;   /* byte 0x00 */
    EXPECT_EQ(-1, hl_exec_policy_validate(b.data, b.len, NULL, 0));
    hl_exec_buf_free(&b);
}

UTEST(exec_validate, wrong_magic)
{
    static const uint8_t junk[] = { 'H', 'X', 'P', '2', 0, 0 };
    EXPECT_EQ(-1, hl_exec_policy_validate(junk, sizeof junk, NULL, 0));
    EXPECT_EQ(-1, hl_exec_policy_validate(NULL, 0, NULL, 0));
}

UTEST(exec_names, valid_names)
{
    EXPECT_TRUE(hl_exec_name_valid(S("gpu-query")));
    EXPECT_TRUE(hl_exec_name_valid(S("a")));
    EXPECT_TRUE(hl_exec_name_valid(S("9x")));
    EXPECT_FALSE(hl_exec_name_valid(S("-a")));
    EXPECT_FALSE(hl_exec_name_valid(S("a_b")));
    EXPECT_FALSE(hl_exec_name_valid(S("A")));
    EXPECT_FALSE(hl_exec_name_valid("", 0));
    char longname[66];
    memset(longname, 'a', 65);
    EXPECT_FALSE(hl_exec_name_valid(longname, 65));
    EXPECT_TRUE(hl_exec_name_valid(longname, 64));
}

/* ── The sha256 pin (PR 4) ─────────────────────────────────────────── */

static int build_pinned(HlExecBuf *b, const char *hex, size_t hex_len)
{
    hl_exec_b_init(b, NULL);
    hl_exec_b_cmd(b, S("gpu"), S("/usr/bin/nvidia-smi"), -1, -1, -1, -1, 0,
                  hex, hex_len);
    for (int k = 0; k < 4; k++) hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
    return hl_exec_b_finish(b);
}

UTEST(exec_pin, a_pin_is_encoded_and_read_back)
{
    static const char hex[] =
        "00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff";
    HlExecBuf b;
    ASSERT_EQ(0, build_pinned(&b, hex, 64));
    HlExecCmd c;
    ASSERT_EQ(0, hl_exec_policy_at(b.data, b.len, 0, &c));
    ASSERT_TRUE(c.sha256 != NULL);
    EXPECT_TRUE(c.flags & HL_EXEC_CMD_SHA256);
    for (int i = 0; i < 32; i++)
        EXPECT_EQ((i % 16) * 0x11, c.sha256[i]);
    /* what follows the pin still reads: no env, fs or argv */
    EXPECT_EQ(0, c.n_env);
    EXPECT_EQ(0, c.n_argv);
    EXPECT_EQ(0, hl_exec_policy_validate(b.data, b.len, NULL, 0));
    hl_exec_buf_free(&b);
}

UTEST(exec_pin, no_pin_reads_as_null)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_pinned(&b, NULL, 0));
    HlExecCmd c;
    ASSERT_EQ(0, hl_exec_policy_at(b.data, b.len, 0, &c));
    EXPECT_TRUE(c.sha256 == NULL);
    EXPECT_FALSE(c.flags & HL_EXEC_CMD_SHA256);
    hl_exec_buf_free(&b);
}

UTEST(exec_pin, a_malformed_pin_fails_the_build)
{
    HlExecBuf b;
    char hex[66];
    memset(hex, 'a', sizeof hex);
    EXPECT_EQ(-1, build_pinned(&b, hex, 63));
    EXPECT_TRUE(strstr(b.err, "64 hex") != NULL);
    hl_exec_buf_free(&b);
    EXPECT_EQ(-1, build_pinned(&b, hex, 65));
    hl_exec_buf_free(&b);
    hex[10] = 'g';
    EXPECT_EQ(-1, build_pinned(&b, hex, 64));
    hl_exec_buf_free(&b);
    EXPECT_EQ(-1, build_pinned(&b, "", 0));
    hl_exec_buf_free(&b);
}

UTEST(exec_pin, a_truncated_pin_is_refused)
{
    static const char hex[] =
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    HlExecBuf b;
    ASSERT_EQ(0, build_pinned(&b, hex, 64));
    /* the flag claims a pin the bytes do not hold */
    for (size_t cut = 1; cut < b.len; cut++)
        EXPECT_EQ(-1, hl_exec_policy_validate(b.data, b.len - cut, NULL, 0));
    hl_exec_buf_free(&b);
}

UTEST_MAIN()
