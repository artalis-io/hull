/*
 * test_smtp_audit.c - exact SMTP terminal-audit records (regression evidence).
 *
 * hl_smtp_audit_complete is the single completion-audit writer shared by the sync
 * and model-2 async paths. It emits ONE JSONL line to stderr (gated by
 * hl_audit_enabled) carrying the stable public fields plus the FROZEN terminal /
 * teardown / schedule tags. These tests capture that line and assert the EXACT
 * record for each tag - including teardown:leaked, which is hard to force through a
 * live transport - and that exactly one line is emitted per call (the record-level
 * half of the no-duplicate-audit guarantee; the live cancel-vs-completion race is
 * covered by tests/e2e_smtp.sh's grep -c == 1).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/cap/smtp.h"
#include "hull/cap/audit.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern int hl_audit_enabled;

/* Run hl_smtp_audit_complete with stderr redirected to a temp file; return the
 * captured bytes in @p out (NUL-terminated). */
static void audit_capture(const HlSmtpMessage *m, const HlSmtpResult *r,
                          const char *schedule, const char *terminal,
                          char *out, size_t cap)
{
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    char tmpl[] = "/tmp/hull_smtp_audit_XXXXXX";
    int fd = mkstemp(tmpl);
    dup2(fd, STDERR_FILENO);

    int prev = hl_audit_enabled;
    hl_audit_enabled = 1;
    hl_smtp_audit_complete(m, r, schedule, terminal);
    hl_audit_enabled = prev;

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, out, cap - 1);
    if (n < 0) n = 0;
    out[n] = '\0';
    close(fd);
    unlink(tmpl);
}

static HlSmtpMessage audit_msg(void)
{
    HlSmtpMessage m; memset(&m, 0, sizeof m);
    m.host = "mail.example.com"; m.from = "s@example.com"; m.to = "r@example.com";
    m.subject = "hi"; m.body = "b";
    return m;
}

static int line_count(const char *s)
{
    int n = 0; for (const char *p = s; *p; p++) if (*p == '\n') n++;
    return n;
}

/* terminal:cancelled - the sweep/cancel path. */
UTEST(smtp_audit, terminal_cancelled_record)
{
    HlSmtpMessage m = audit_msg();
    HlSmtpResult  r; memset(&r, 0, sizeof r);
    r.rc = -1; r.token = "connect_failed";
    char buf[1024];
    audit_capture(&m, &r, NULL, "cancelled", buf, sizeof buf);

    ASSERT_TRUE(strstr(buf, "\"cap\":\"smtp.send\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"terminal\":\"cancelled\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"result\":-1") != NULL);
    ASSERT_TRUE(strstr(buf, "\"host\":\"mail.example.com\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"teardown\"") == NULL);   /* no teardown tag here */
    ASSERT_EQ(line_count(buf), 1);                      /* exactly one record */
}

/* terminal:post_resolution_deadline - a Dop expiry (section 8). */
UTEST(smtp_audit, terminal_post_resolution_deadline_record)
{
    HlSmtpMessage m = audit_msg();
    HlSmtpResult  r; memset(&r, 0, sizeof r);
    r.rc = -1; r.token = "connect_failed"; r.deadline_expired = 1;
    char buf[1024];
    audit_capture(&m, &r, NULL, "post_resolution_deadline", buf, sizeof buf);

    ASSERT_TRUE(strstr(buf, "\"terminal\":\"post_resolution_deadline\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"cap\":\"smtp.send\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"terminal\":\"cancelled\"") == NULL);  /* not mis-tagged */
    ASSERT_EQ(line_count(buf), 1);
}

/* teardown:leaked - transport teardown could not confirm detachment. */
UTEST(smtp_audit, teardown_leaked_record)
{
    HlSmtpMessage m = audit_msg();
    HlSmtpResult  r; memset(&r, 0, sizeof r);
    r.rc = -1; r.token = "connect_failed"; r.teardown_leaked = 1;
    char buf[1024];
    audit_capture(&m, &r, NULL, NULL, buf, sizeof buf);

    ASSERT_TRUE(strstr(buf, "\"teardown\":\"leaked\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"cap\":\"smtp.send\"") != NULL);
    ASSERT_EQ(line_count(buf), 1);
}

/* schedule:cap_reached (+ teardown) compose into one record. */
UTEST(smtp_audit, schedule_cap_reached_record)
{
    HlSmtpMessage m = audit_msg();
    HlSmtpResult  r; memset(&r, 0, sizeof r);
    r.rc = -1; r.token = "connect_failed";
    char buf[1024];
    audit_capture(&m, &r, "cap_reached", NULL, buf, sizeof buf);

    ASSERT_TRUE(strstr(buf, "\"schedule\":\"cap_reached\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"terminal\"") == NULL);
    ASSERT_EQ(line_count(buf), 1);
}

/* A normal completion (no schedule, no terminal, no teardown) carries none of the
 * failure tags and remains a single record. */
UTEST(smtp_audit, ordinary_completion_record_has_no_failure_tags)
{
    HlSmtpMessage m = audit_msg();
    HlSmtpResult  r; memset(&r, 0, sizeof r);
    r.rc = 0; r.token = NULL;
    char buf[1024];
    audit_capture(&m, &r, NULL, NULL, buf, sizeof buf);

    ASSERT_TRUE(strstr(buf, "\"result\":0") != NULL);
    ASSERT_TRUE(strstr(buf, "\"terminal\"") == NULL);
    ASSERT_TRUE(strstr(buf, "\"schedule\"") == NULL);
    ASSERT_TRUE(strstr(buf, "\"teardown\"") == NULL);
    ASSERT_EQ(line_count(buf), 1);
}

UTEST_MAIN()
