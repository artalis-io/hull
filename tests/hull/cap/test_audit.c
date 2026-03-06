/*
 * test_audit.c — Tests for capability audit logging
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/audit.h"
#include <string.h>
#include <unistd.h>

/* ── Gating: disabled audit produces no output ───────────────────── */

UTEST(hl_audit, disabled_is_noop)
{
    hl_audit_enabled = 0;

    ShJsonWriter w = hl_audit_begin("test.noop");
    /* Writer should have error=1 when disabled */
    ASSERT_EQ(w.error, 1);

    /* Writes should be no-ops (not crash, not produce output) */
    sh_json_write_kv_string(&w, "key", "value");
    hl_audit_end(&w);
}

/* ── Output: enabled audit writes JSON to stderr ─────────────────── */

UTEST(hl_audit, enabled_writes_json)
{
    /* Capture stderr via pipe */
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    int saved_stderr = dup(STDERR_FILENO);
    ASSERT_NE(saved_stderr, -1);
    dup2(pipefd[1], STDERR_FILENO);

    hl_audit_enabled = 1;

    ShJsonWriter w = hl_audit_begin("test.cap");
    ASSERT_EQ(w.error, 0);
    sh_json_write_kv_string(&w, "name", "hello");
    sh_json_write_kv_int(&w, "count", 42);
    hl_audit_end(&w);

    /* Flush and restore stderr */
    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    close(pipefd[1]);

    /* Read captured output */
    char buf[2048];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    ASSERT_GT(n, 0);
    buf[n] = '\0';

    /* Verify JSON structure */
    ASSERT_NE(strstr(buf, "\"ts\":"), NULL);
    ASSERT_NE(strstr(buf, "\"cap\":\"test.cap\""), NULL);
    ASSERT_NE(strstr(buf, "\"name\":\"hello\""), NULL);
    ASSERT_NE(strstr(buf, "\"count\":42"), NULL);

    /* Should start with { and end with }\n */
    ASSERT_EQ(buf[0], '{');
    ASSERT_EQ(buf[n - 1], '\n');
    ASSERT_EQ(buf[n - 2], '}');

    hl_audit_enabled = 0;
}

/* ── Escaping: special characters are properly escaped ───────────── */

UTEST(hl_audit, escaping)
{
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    int saved_stderr = dup(STDERR_FILENO);
    ASSERT_NE(saved_stderr, -1);
    dup2(pipefd[1], STDERR_FILENO);

    hl_audit_enabled = 1;

    ShJsonWriter w = hl_audit_begin("test.esc");
    sh_json_write_kv_string(&w, "val", "line1\nline2");
    sh_json_write_kv_string(&w, "quot", "say \"hi\"");
    sh_json_write_kv_string(&w, "back", "a\\b");
    hl_audit_end(&w);

    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    close(pipefd[1]);

    char buf[2048];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    ASSERT_GT(n, 0);
    buf[n] = '\0';

    /* sh_json escapes \n, ", and \ in strings */
    ASSERT_NE(strstr(buf, "\\n"), NULL);
    ASSERT_NE(strstr(buf, "\\\"hi\\\""), NULL);
    ASSERT_NE(strstr(buf, "a\\\\b"), NULL);

    hl_audit_enabled = 0;
}

UTEST_MAIN()
