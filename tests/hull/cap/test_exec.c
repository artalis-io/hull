/*
 * test_exec.c - the declared-command policy and the argument matcher
 * (cap/exec.c, docs/design/exec_broker.md §3 / §4).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/exec.h"
#include "hull/cap/exec_child.h"
#include "hull/cap/exec_broker.h"
#include "hull/shared/fs_util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

#define S(x) x, sizeof(x) - 1

/* unit-state from the design doc: fixed args, `--`, then a repeat slot. */
static int build_unit_state(HlExecBuf *b, int64_t rmin, int64_t rmax)
{
    hl_exec_b_init(b, NULL);
    hl_exec_b_cmd(b, S("unit-state"), S("/usr/bin/systemctl"), -1, 64, -1, -1, 1, -1, NULL, 0);
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
    hl_exec_b_cmd(b, S("cmd"), S("/usr/bin/tool"), -1, -1, -1, -1, 0, -1, NULL, 0);
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
                  c->timeout, c->so, c->se, c->conc, 0, -1, NULL, 0);
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
        hl_exec_b_cmd(&b, name, strlen(name), S("/bin/true"), -1, -1, -1, -1, 0, -1, NULL, 0);
        for (int k = 0; k < 4; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    }
    EXPECT_EQ(-1, hl_exec_b_finish(&b));
    hl_exec_b_init(&b, NULL);
    for (int i = 0; i < 2; i++) {
        hl_exec_b_cmd(&b, S("same"), S("/bin/true"), -1, -1, -1, -1, 0, -1, NULL, 0);
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
    hl_exec_b_cmd(b, S("gpu"), S("/usr/bin/nvidia-smi"), -1, -1, -1, -1, 0, -1,
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

/* ── stdin_max ─────────────────────────────────────────────────────── */

/* One command with @p stdin_max and, when @p hex, a pin before it. */
static int build_stdin(HlExecBuf *b, int64_t stdin_max, const char *hex)
{
    hl_exec_b_init(b, NULL);
    hl_exec_b_cmd(b, S("key-import"), S("/usr/bin/tool"), -1, -1, -1, -1, 0,
                  stdin_max, hex, hex ? 64 : 0);
    for (int k = 0; k < 4; k++) hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
    return hl_exec_b_finish(b);
}

static const char PIN_HEX[] =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

UTEST(exec_stdin, stdin_max_is_encoded_and_read_back)
{
    HlExecBuf b;
    HlExecCmd c;
    ASSERT_EQ(0, build_stdin(&b, 4096, NULL));
    ASSERT_EQ(0, hl_exec_policy_at(b.data, b.len, 0, &c));
    EXPECT_TRUE(c.flags & HL_EXEC_CMD_STDIN);
    EXPECT_EQ(4096u, c.stdin_max);
    EXPECT_EQ(0, c.n_argv);
    hl_exec_buf_free(&b);
    /* after a pin: both read back, and what follows still reads */
    ASSERT_EQ(0, build_stdin(&b, (int64_t)HL_EXEC_STDIN_CEIL, PIN_HEX));
    ASSERT_EQ(0, hl_exec_policy_at(b.data, b.len, 0, &c));
    ASSERT_TRUE(c.sha256 != NULL);
    EXPECT_EQ(0x11, c.sha256[1]);
    EXPECT_EQ(HL_EXEC_STDIN_CEIL, c.stdin_max);
    EXPECT_EQ(0, c.n_env);
    hl_exec_buf_free(&b);
}

UTEST(exec_stdin, no_stdin_max_means_dev_null)
{
    HlExecBuf b;
    HlExecCmd c;
    for (int64_t v = -1; v <= 0; v++) {      /* not given, or 0 */
        ASSERT_EQ(0, build_stdin(&b, v, NULL));
        ASSERT_EQ(0, hl_exec_policy_at(b.data, b.len, 0, &c));
        EXPECT_FALSE(c.flags & HL_EXEC_CMD_STDIN);
        EXPECT_EQ(0u, c.stdin_max);
        hl_exec_buf_free(&b);
    }
}

UTEST(exec_stdin, stdin_max_out_of_range_fails_the_build)
{
    HlExecBuf b;
    EXPECT_EQ(-1, build_stdin(&b, (int64_t)HL_EXEC_STDIN_CEIL + 1, NULL));
    EXPECT_TRUE(strstr(b.err, "stdin_max") != NULL);
    EXPECT_EQ(-1, build_stdin(&b, -5, NULL));    /* a negative value as written */
    EXPECT_TRUE(strstr(b.err, "stdin_max") != NULL);
}

UTEST(exec_stdin, a_truncated_stdin_max_is_refused)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_stdin(&b, 64, NULL));
    /* cut inside the u32 that follows the flags byte */
    const char *name = "key-import", *path = "/usr/bin/tool";
    size_t at = 4 + 2 + (2 + strlen(name) + 1) + (2 + strlen(path) + 1) + 12 + 2;
    for (size_t cut = at; cut < at + 4; cut++)
        EXPECT_EQ(-1, hl_exec_policy_validate(b.data, cut, NULL, 0));
    hl_exec_buf_free(&b);
}

/* ── The child manifest (exec.<name>.manifest, §3) ─────────────────── */

/* gpu-query as v0.26.0 encodes it (host fs + unix_sockets, no child
 * manifest): captured from the v0.26.0 cap/exec.o. A command with no child
 * manifest must encode to exactly these bytes, so a signed v0.26.0 policy
 * still means what it meant. */
static const uint8_t V026_GPU_QUERY[] = {
    0x48, 0x58, 0x50, 0x31, 0x01, 0x00, 0x09, 0x00, 0x67, 0x70, 0x75, 0x2d,
    0x71, 0x75, 0x65, 0x72, 0x79, 0x00, 0x13, 0x00, 0x2f, 0x75, 0x73, 0x72,
    0x2f, 0x62, 0x69, 0x6e, 0x2f, 0x6e, 0x76, 0x69, 0x64, 0x69, 0x61, 0x2d,
    0x73, 0x6d, 0x69, 0x00, 0x88, 0x13, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x01, 0x00, 0x0e, 0x00,
    0x2f, 0x64, 0x65, 0x76, 0x2f, 0x6e, 0x76, 0x69, 0x64, 0x69, 0x61, 0x63,
    0x74, 0x6c, 0x00, 0x01, 0x00, 0x0e, 0x00, 0x2f, 0x64, 0x65, 0x76, 0x2f,
    0x6e, 0x76, 0x69, 0x64, 0x69, 0x61, 0x63, 0x74, 0x6c, 0x00, 0x01, 0x00,
    0x00, 0x0c, 0x00, 0x2d, 0x2d, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x3d,
    0x63, 0x73, 0x76, 0x00,
};

static int build_gpu_query(HlExecBuf *b, int child)
{
    hl_exec_b_init(b, NULL);
    hl_exec_b_cmd(b, S("gpu-query"), S("/usr/bin/nvidia-smi"), 5000, 65536, -1, -1,
                  1, -1, NULL, 0);
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);              /* env */
    size_t m = hl_exec_b_count_begin(b);                              /* host.read */
    hl_exec_b_str(b, S("/dev/nvidiactl"));
    hl_exec_b_count_end(b, m, 1);
    m = hl_exec_b_count_begin(b);                                     /* host.write */
    hl_exec_b_str(b, S("/dev/nvidiactl"));
    hl_exec_b_count_end(b, m, 1);
    m = hl_exec_b_count_begin(b);                                     /* argv */
    hl_exec_b_fixed(b, S("--format=csv"));
    hl_exec_b_count_end(b, m, 1);
    if (child) {                                                      /* manifest.fs */
        hl_exec_b_app_fs(b);
        m = hl_exec_b_count_begin(b);
        hl_exec_b_str(b, S("data/gpu-ids"));
        hl_exec_b_str(b, S("@state/cache/"));
        hl_exec_b_count_end(b, m, 2);
        m = hl_exec_b_count_begin(b);
        hl_exec_b_str(b, S("out/"));
        hl_exec_b_count_end(b, m, 1);
    }
    return hl_exec_b_finish(b);
}

UTEST(exec_child, no_child_manifest_encodes_as_v0_26_0)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_gpu_query(&b, 0));
    ASSERT_EQ(sizeof V026_GPU_QUERY, b.len);
    EXPECT_EQ(0, memcmp(V026_GPU_QUERY, b.data, b.len));
    /* and v0.26.0's bytes still validate and read the same */
    HlExecCmd c;
    ASSERT_EQ(0, hl_exec_policy_validate(V026_GPU_QUERY, sizeof V026_GPU_QUERY, NULL, 0));
    ASSERT_EQ(0, hl_exec_policy_at(V026_GPU_QUERY, sizeof V026_GPU_QUERY, 0, &c));
    EXPECT_TRUE(c.flags & HL_EXEC_CMD_UNIX_SOCKETS);
    EXPECT_FALSE(c.flags & HL_EXEC_CMD_APP_FS);
    EXPECT_EQ(1, c.n_fs_read);
    EXPECT_EQ(0, c.n_app_read);
    hl_exec_buf_free(&b);
}

UTEST(exec_child, manifest_fs_is_encoded_and_read_back)
{
    HlExecBuf b;
    HlExecCmd c;
    ASSERT_EQ(0, build_gpu_query(&b, 1));
    ASSERT_EQ(0, hl_exec_policy_find(b.data, b.len, S("gpu-query"), &c));
    EXPECT_TRUE(c.flags & HL_EXEC_CMD_APP_FS);
    EXPECT_EQ(1, c.n_fs_read);             /* the host grants are unchanged */
    EXPECT_EQ(1, c.n_argv);
    ASSERT_EQ(2, c.n_app_read);
    ASSERT_EQ(1, c.n_app_write);
    const uint8_t *cur = c.app_read;
    EXPECT_STREQ("data/gpu-ids", hl_exec_next_str(&cur, NULL));
    EXPECT_STREQ("@state/cache/", hl_exec_next_str(&cur, NULL));
    cur = c.app_write;
    EXPECT_STREQ("out/", hl_exec_next_str(&cur, NULL));
    /* every truncation inside the child lists is refused */
    for (size_t cut = sizeof V026_GPU_QUERY; cut < b.len; cut++)
        EXPECT_EQ(-1, hl_exec_policy_validate(b.data, cut, NULL, 0));
    hl_exec_buf_free(&b);
}

UTEST(exec_child, an_absolute_child_entry_is_refused_by_validation)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    hl_exec_b_cmd(&b, S("c"), S("/bin/cat"), -1, -1, -1, -1, 0, -1, NULL, 0);
    for (int k = 0; k < 4; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    hl_exec_b_app_fs(&b);
    size_t m = hl_exec_b_count_begin(&b);
    hl_exec_b_str(&b, S("/etc/passwd"));
    hl_exec_b_count_end(&b, m, 1);
    hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    EXPECT_EQ(-1, hl_exec_b_finish(&b));
    EXPECT_TRUE(strstr(b.err, "manifest.fs.read entry 1") != NULL);
}

UTEST(exec_child, grants_a_command_may_take)
{
    char why[160];
    static const char *const ok[] = { "data/gpu-ids", "data/", "./data/x.csv",
                                      "@state/cache", "@state/cache/", NULL };
    for (int i = 0; ok[i]; i++)
        EXPECT_EQ(0, hl_exec_child_grant_check(ok[i], strlen(ok[i]), why, sizeof why));
    /* valid for an app, refused for a command, each with its reason */
    static const struct { const char *g, *why; } bad[] = {
        { ".", "whole app directory" },
        { "./", "whole app directory" },
        { "@state", "whole state directory" },
        { "data/*.csv", "pattern" },
        { "*.log", "pattern" },
        /* invalid anywhere */
        { "/etc/passwd", "not an fs grant" },
        { "../x", "not an fs grant" },
        { "data/../../x", "not an fs grant" },
        { ".hull/internal.db", "reserved" },
        { "@other/x", "reserved" },
        { NULL, NULL } };
    for (int i = 0; bad[i].g; i++) {
        EXPECT_EQ(-1, hl_exec_child_grant_check(bad[i].g, strlen(bad[i].g), why,
                                                sizeof why));
        EXPECT_TRUE(strstr(why, bad[i].why) != NULL);
    }
}

UTEST(exec_child, resolve_names_exactly_what_the_entry_names)
{
    char root[] = "/tmp/hull-exec-child-XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);
    char app[PATH_MAX], state[PATH_MAX], p[PATH_MAX + 64], out[PATH_MAX], why[160];
    snprintf(app, sizeof app, "%s/app", root);
    snprintf(state, sizeof state, "%s/state", root);
    ASSERT_EQ(0, mkdir(app, 0700));
    ASSERT_EQ(0, mkdir(state, 0700));
    snprintf(p, sizeof p, "%s/data", app);
    ASSERT_EQ(0, mkdir(p, 0700));
    snprintf(p, sizeof p, "%s/data/f.txt", app);
    FILE *f = fopen(p, "w");
    ASSERT_TRUE(f != NULL);
    fclose(f);
    snprintf(p, sizeof p, "%s/link", app);
    ASSERT_EQ(0, symlink("/etc", p));

    char real_app[PATH_MAX], want[PATH_MAX + 64];
    ASSERT_TRUE(realpath(app, real_app) != NULL);
    /* a file: the file itself, never its parent */
    ASSERT_EQ(0, hl_exec_child_grant_resolve(app, NULL, "data/f.txt", out, sizeof out,
                                             why, sizeof why));
    snprintf(want, sizeof want, "%s/data/f.txt", real_app);
    EXPECT_STREQ(want, out);
    /* a directory to create (trailing slash), as the app's sandbox does */
    ASSERT_EQ(0, hl_exec_child_grant_resolve(app, NULL, "out/", out, sizeof out,
                                             why, sizeof why));
    struct stat st;
    snprintf(p, sizeof p, "%s/out", app);
    EXPECT_EQ(0, stat(p, &st));
    /* refused: a missing file, a symbolic link, @state with no state, a pattern */
    EXPECT_EQ(-1, hl_exec_child_grant_resolve(app, NULL, "data/missing.txt", out,
                                              sizeof out, why, sizeof why));
    EXPECT_TRUE(strstr(why, "does not exist") != NULL);
    EXPECT_EQ(-1, hl_exec_child_grant_resolve(app, NULL, "link/passwd", out,
                                              sizeof out, why, sizeof why));
    EXPECT_TRUE(strstr(why, "symbolic link") != NULL);
    EXPECT_EQ(-1, hl_exec_child_grant_resolve(app, NULL, "@state/x/", out, sizeof out,
                                              why, sizeof why));
    EXPECT_TRUE(strstr(why, "no state directory") != NULL);
    EXPECT_EQ(-1, hl_exec_child_grant_resolve(app, NULL, "data/*.txt", out, sizeof out,
                                              why, sizeof why));
    /* @state/ resolves under the state root */
    ASSERT_EQ(0, hl_exec_child_grant_resolve(app, state, "@state/cache/", out,
                                             sizeof out, why, sizeof why));
    EXPECT_TRUE(strstr(out, "/state/cache") != NULL);

    /* Remove exactly what the test made, deepest first. */
    static const char *const made[] = { "app/data/f.txt", "app/link", "app/data",
                                        "app/out", "state/cache", "app", "state", NULL };
    for (int i = 0; made[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", root, made[i]);
        struct stat ls;
        if (lstat(p, &ls) == 0) EXPECT_EQ(0, S_ISDIR(ls.st_mode) ? rmdir(p) : unlink(p));
    }
    EXPECT_EQ(0, rmdir(root));
}

/* Review findings on the child grant (each failed before its fix). */
static void mk(const char *root, const char *rel, int dir)
{
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    if (dir) { (void)mkdir(p, 0700); return; }
    FILE *f = fopen(p, "w");
    if (f) { fputs("x", f); fclose(f); }
}

static void rm_list(const char *root, const char *const *rel)
{
    char p[PATH_MAX + 64];
    for (int i = 0; rel[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", root, rel[i]);
        struct stat ls;
        if (lstat(p, &ls) == 0) (void)(S_ISDIR(ls.st_mode) ? rmdir(p) : unlink(p));
    }
    (void)rmdir(root);
}

UTEST(exec_child, review_broad_root_nested_hull_db_and_binding)
{
    char root[] = "/tmp/hull-exec-review-XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);
    char app[PATH_MAX], why[192], out[PATH_MAX];
    snprintf(app, sizeof app, "%s/app", root);
    mk(root, "app", 1);
    mk(app, ".hull", 1);
    mk(app, "st", 1);              /* a state directory inside the app directory */
    mk(app, "st/.hull", 1);
    mk(app, "st/.hull/internal.db", 0);
    mk(app, "data", 1);
    mk(app, "data/app.db", 0);
    mk(app, "data/app.db-shm", 0);
    mk(app, "data/f.txt", 0);
    char st[PATH_MAX + 8];
    snprintf(st, sizeof st, "%s/st", app);

    /* Q1: "/" (or the home) as the app root is not an app directory */
    EXPECT_EQ(-1, hl_exec_child_grant_resolve("/", NULL, "tmp/", out, sizeof out,
                                              why, sizeof why));
    EXPECT_TRUE(strstr(why, "not an app directory") != NULL);
    /* Q2: the state root nested in the app root: neither its directory nor
     * anything through its .hull */
    EXPECT_EQ(-1, hl_exec_child_grant_resolve(app, st, "st/", out, sizeof out,
                                              why, sizeof why));
    EXPECT_TRUE(strstr(why, "holds the state directory") != NULL);
    EXPECT_EQ(-1, hl_exec_child_grant_resolve(app, st, "st/.hull/internal.db", out,
                                              sizeof out, why, sizeof why));
    EXPECT_TRUE(strstr(why, ".hull") != NULL);
    /* Q2 (re-review): a .hull that does not exist yet at load. The walk
     * reaches the state root and would create its .hull: it is compared
     * with the root's .hull as it is after the mkdirat, refused, and the
     * directory it made is removed (on HFS+ an ignorable-code-point name is
     * the same directory; APFS shows the same path with the plain name). */
    mk(app, "st2", 1);
    char st2[PATH_MAX + 8], st2h[PATH_MAX + 16];
    snprintf(st2, sizeof st2, "%s/st2", app);
    snprintf(st2h, sizeof st2h, "%s/.hull", st2);
    EXPECT_EQ(-1, hl_exec_child_grant_resolve(app, st2, "st2/.hull/x/", out, sizeof out,
                                              why, sizeof why));
    EXPECT_TRUE(strstr(why, ".hull") != NULL);
    struct stat gone;
    EXPECT_NE(0, lstat(st2h, &gone));          /* not left behind */
    /* Q2: a write grant may not reach the database or its sidecars; a read may */
    const char *protect[1] = { "data/app.db" };
    HlExecChildRoots r = { app, NULL, protect, 1 };
    HlExecChildGrant g;
    EXPECT_EQ(-1, hl_exec_child_grant_open(&r, "data/", 1, &g, why, sizeof why));
    EXPECT_TRUE(strstr(why, "contains the app's database") != NULL);
    EXPECT_EQ(-1, hl_exec_child_grant_open(&r, "data/app.db-shm", 1, &g, why, sizeof why));
    EXPECT_TRUE(strstr(why, "sidecar") != NULL);
    ASSERT_EQ(0, hl_exec_child_grant_open(&r, "data/", 0, &g, why, sizeof why));
    close(g.fd);
    /* Q3: the grant is the opened object: a later swap of a component for a
     * symbolic link does not change what the descriptor names */
    ASSERT_EQ(0, hl_exec_child_grant_open(&r, "data/f.txt", 0, &g, why, sizeof why));
    char from[PATH_MAX + 16], to[PATH_MAX + 16];
    snprintf(from, sizeof from, "%s/data", app);
    snprintf(to, sizeof to, "%s/data2", app);
    ASSERT_EQ(0, rename(from, to));
    ASSERT_EQ(0, symlink("/etc", from));
    struct stat a, b;
    char moved[PATH_MAX + 32];
    snprintf(moved, sizeof moved, "%s/data2/f.txt", app);
    ASSERT_EQ(0, fstat(g.fd, &a));
    ASSERT_EQ(0, stat(moved, &b));
    EXPECT_TRUE(a.st_ino == b.st_ino && a.st_dev == b.st_dev);
    close(g.fd);
    /* and resolving again now refuses the link */
    EXPECT_EQ(-1, hl_exec_child_grant_resolve(app, NULL, "data/f.txt", out, sizeof out,
                                              why, sizeof why));
    EXPECT_TRUE(strstr(why, "symbolic link") != NULL);

    static const char *const made[] = { "app/data2/app.db", "app/data2/app.db-shm",
        "app/data2/f.txt", "app/data2", "app/data", "app/st/.hull/internal.db",
        "app/st/.hull", "app/st", "app/st2", "app/.hull", "app", NULL };
    rm_list(root, made);
}

/* Final review: a first start, an unresolvable app directory, DuckDB. */
UTEST(exec_child, final_first_start_unresolvable_root_and_duckdb)
{
    char root[] = "/tmp/hull-exec-final-XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);
    char app[PATH_MAX], rapp[PATH_MAX], why[192];
    snprintf(app, sizeof app, "%s/app", root);
    mk(root, "app", 1);
    ASSERT_TRUE(realpath(app, rapp) != NULL);

    /* First start: the database's directory does not exist yet. The server
     * sends the path it will have; the broker refuses a write grant that
     * would hold it, by path, before the database is ever opened. */
    char fresh[PATH_MAX + 32];
    snprintf(fresh, sizeof fresh, "%s/data/sub/other.db", rapp);
    const char *p1[1] = { fresh };
    HlExecChildRoots r1 = { app, NULL, p1, 1 };
    HlExecChildGrant g;
    EXPECT_EQ(-1, hl_exec_child_grant_open(&r1, "data/", 1, &g, why, sizeof why));
    EXPECT_TRUE(strstr(why, "contains the app's database") != NULL);
    /* the same check from the path the server computes */
    char resolved[PATH_MAX];
    char rel[PATH_MAX + 32];
    snprintf(rel, sizeof rel, "%s/nope/sub/x.db", app);
    ASSERT_EQ(0, hl_path_resolve_partial(rel, resolved));
    char want[PATH_MAX + 32];
    snprintf(want, sizeof want, "%s/nope/sub/x.db", rapp);
    EXPECT_STREQ(want, resolved);
    snprintf(rel, sizeof rel, "%s/nope/../x.db", app);
    EXPECT_EQ(-1, hl_path_resolve_partial(rel, resolved));

    /* DuckDB: its .wal and its .tmp directory are the database's too */
    mk(app, "duck", 1);
    mk(app, "duck/app.duckdb", 0);
    mk(app, "duck/app.duckdb.wal", 0);
    mk(app, "duck/app.duckdb.tmp", 1);
    char duck[PATH_MAX + 32];
    snprintf(duck, sizeof duck, "%s/duck/app.duckdb", rapp);
    const char *p2[1] = { duck };
    HlExecChildRoots r2 = { app, NULL, p2, 1 };
    EXPECT_EQ(-1, hl_exec_child_grant_open(&r2, "duck/app.duckdb.wal", 1, &g, why,
                                           sizeof why));
    EXPECT_EQ(-1, hl_exec_child_grant_open(&r2, "duck/app.duckdb.tmp/", 1, &g, why,
                                           sizeof why));

    /* An app directory that cannot be resolved: refused before any broker
     * (none runs in this test), when a command has an app-relative grant. */
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    hl_exec_b_cmd(&b, S("c"), S("/bin/cat"), -1, -1, -1, -1, 0, -1, NULL, 0);
    for (int k = 0; k < 4; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    hl_exec_b_app_fs(&b);
    size_t m = hl_exec_b_count_begin(&b);
    hl_exec_b_str(&b, S("data/"));
    hl_exec_b_count_end(&b, m, 1);
    hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    ASSERT_EQ(0, hl_exec_b_finish(&b));
    EXPECT_EQ(-1, hl_exec_broker_apply(b.data, b.len, 1, 0, "/nonexistent-hull-app-dir",
                                       NULL, NULL, NULL));
    hl_exec_buf_free(&b);

    static const char *const made[] = { "app/duck/app.duckdb.tmp", "app/duck/app.duckdb.wal",
        "app/duck/app.duckdb", "app/duck", "app/data", "app", NULL };
    rm_list(root, made);
}

/* Audit 53: a grant naming a control byte is refused (the broker's report
 * quotes it in a log line), and a directory grant whose containment of a
 * root cannot be judged is refused, never taken as "not contained". */
UTEST(exec_child_audit53, a_control_byte_is_refused)
{
    char why[160];
    static const char *const bad[] = { "data/a\nX", "data/\rx", "data/\x1b[2K", "d\x7f", NULL };
    for (int i = 0; bad[i]; i++) {
        EXPECT_EQ(-1, hl_exec_child_grant_check(bad[i], strlen(bad[i]), why, sizeof why));
        EXPECT_TRUE(strstr(why, "control") != NULL);
    }
    /* the load error names the entry, not the bytes */
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    EXPECT_EQ(-1, hl_exec_child_grant_entry(&b, "c", "read", 1, "data/a\nX", 8));
    EXPECT_TRUE(b.err[0] && strchr(b.err, '\n') == NULL);
    hl_exec_buf_free(&b);
}

UTEST(exec_child_audit53, unwalkable_root_containment_is_refused)
{
    if (geteuid() == 0) { UTEST_SKIP("root ignores the directory mode"); }
    char root[] = "/tmp/hull-exec-a53-XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);
    char app[PATH_MAX], mid[PATH_MAX + 16], st[PATH_MAX + 32], why[192];
    snprintf(app, sizeof app, "%s/app", root);
    mk(root, "app", 1);
    mk(app, "var", 1);
    mk(app, "var/mid", 1);
    mk(app, "var/mid/st", 1);
    snprintf(mid, sizeof mid, "%s/var/mid", app);
    snprintf(st, sizeof st, "%s/var/mid/st", app);
    HlExecChildRoots r = { app, st, NULL, 0 };
    HlExecChildGrant g;
    /* walkable: refused because it holds the state directory */
    EXPECT_EQ(-1, hl_exec_child_grant_open(&r, "var/", 1, &g, why, sizeof why));
    EXPECT_TRUE(strstr(why, "holds the state directory") != NULL);
    /* not walkable (no search right on mid): still refused */
    ASSERT_EQ(0, chmod(mid, 0));
    int rc = hl_exec_child_grant_open(&r, "var/", 1, &g, why, sizeof why);
    if (rc == 0) close(g.fd);
    (void)chmod(mid, 0700);
    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(strstr(why, "cannot be checked") != NULL);
    static const char *const made[] = { "app/var/mid/st", "app/var/mid", "app/var", "app", NULL };
    rm_list(root, made);
}

/* Review of the audit 53 fix: the walk needs only the search right, as a
 * path lookup does, so an app under a directory the service account may pass
 * through but not list (mode 0711) still gets its directory grants. */
UTEST(exec_child_audit53, a_search_only_ancestor_is_walked)
{
#if defined(__COSMOPOLITAN__)
    /* No search-only open here: the walk falls back to O_RDONLY and refuses
     * an unreadable ancestor (fail closed). Cosmo has no broker backend. */
    UTEST_SKIP("no O_PATH / O_SEARCH walk on a cosmo build");
#endif
    if (geteuid() == 0) { UTEST_SKIP("root ignores the directory mode"); }
    char root[] = "/tmp/hull-exec-a53s-XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);
    char top[PATH_MAX], app[PATH_MAX + 16], st[PATH_MAX + 16], why[192];
    snprintf(top, sizeof top, "%s/top", root);
    mk(root, "top", 1);
    mk(top, "app", 1);
    mk(top, "app/out", 1);
    mk(top, "st", 1);
    snprintf(app, sizeof app, "%s/app", top);
    snprintf(st, sizeof st, "%s/st", top);
    ASSERT_EQ(0, chmod(top, 0311));             /* search, no read */
    HlExecChildRoots r = { app, st, NULL, 0 };
    HlExecChildGrant g;
    int rc = hl_exec_child_grant_open(&r, "out/", 1, &g, why, sizeof why);
    if (rc == 0) close(g.fd);
    (void)chmod(top, 0700);
    EXPECT_EQ(0, rc);
    if (rc != 0) printf("        refused: %s\n", why);
    static const char *const made[] = { "top/app/out", "top/app", "top/st", "top", NULL };
    rm_list(root, made);
}

/* ── One command's slice (the macOS router, exec_broker_macos_isolation.md) ── */

static int build_three(HlExecBuf *b)
{
    static const char *const names[] = { "one", "two", "three" };
    static const char *const paths[] = { "/usr/bin/true", "/bin/cat", "/usr/bin/env" };
    hl_exec_b_init(b, NULL);
    for (int i = 0; i < 3; i++) {
        hl_exec_b_cmd(b, names[i], strlen(names[i]), paths[i], strlen(paths[i]),
                      1000 * (i + 1), -1, -1, -1, i == 1, -1, NULL, 0);
        hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);     /* env */
        hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);     /* fs.read */
        size_t m = hl_exec_b_count_begin(b);                     /* fs.write */
        if (i == 2) hl_exec_b_str(b, S("/tmp/x"));
        hl_exec_b_count_end(b, m, i == 2 ? 1 : 0);
        m = hl_exec_b_count_begin(b);                            /* argv */
        hl_exec_b_fixed(b, S("--"));
        hl_exec_b_count_end(b, m, 1);
    }
    return hl_exec_b_finish(b);
}

UTEST(exec_policy_slice, each_command_alone_byte_for_byte)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_three(&b));
    ASSERT_EQ(3, hl_exec_policy_count(b.data, b.len));
    size_t entries = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t *s = NULL;
        size_t sl = 0;
        ASSERT_EQ(0, hl_exec_policy_slice(b.data, b.len, i, &s, &sl));
        char err[256] = "";
        EXPECT_EQ(0, hl_exec_policy_validate(s, sl, err, sizeof err));
        EXPECT_EQ(1, hl_exec_policy_count(s, sl));
        EXPECT_EQ(0, memcmp(s, b.data, 4));           /* the same magic */
        /* Its entry is the original's bytes: the slices, in order, are the
         * whole policy after its header. */
        ASSERT_TRUE(6 + entries + (sl - 6) <= b.len);
        EXPECT_EQ(0, memcmp(s + 6, b.data + 6 + entries, sl - 6));
        entries += sl - 6;
        HlExecCmd orig, one;
        ASSERT_EQ(0, hl_exec_policy_at(b.data, b.len, i, &orig));
        ASSERT_EQ(0, hl_exec_policy_at(s, sl, 0, &one));
        EXPECT_STREQ(orig.name, one.name);
        EXPECT_STREQ(orig.path, one.path);
        EXPECT_EQ(orig.timeout_ms, one.timeout_ms);
        EXPECT_EQ(orig.flags, one.flags);
        EXPECT_EQ(orig.n_fs_write, one.n_fs_write);
        free(s);
    }
    EXPECT_EQ(b.len - 6, entries);
    hl_exec_buf_free(&b);
}

UTEST(exec_policy_slice, out_of_range_and_bad_input)
{
    HlExecBuf b;
    ASSERT_EQ(0, build_three(&b));
    uint8_t *s = NULL;
    size_t sl = 0;
    EXPECT_EQ(-1, hl_exec_policy_slice(b.data, b.len, 3, &s, &sl));
    EXPECT_EQ(-1, hl_exec_policy_slice(b.data, b.len, -1, &s, &sl));
    EXPECT_EQ(-1, hl_exec_policy_slice(b.data, 3, 0, &s, &sl));
    EXPECT_EQ(-1, hl_exec_policy_slice(b.data, b.len, 0, NULL, &sl));
    EXPECT_TRUE(s == NULL);
    hl_exec_buf_free(&b);
}

UTEST_MAIN()
