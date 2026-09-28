/*
 * test_smtp.c - Unit tests for SMTP capability (no network)
 *
 * Tests response parsing, message formatting,
 * host allowlist checking, and CRLF injection guards.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/smtp.h"

#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * Message formatting tests
 * ════════════════════════════════════════════════════════════════════ */

UTEST(format, basic_message)
{
    HlSmtpMessage msg = {
        .from = "sender@example.com",
        .to = "recipient@example.com",
        .subject = "Test Subject",
        .body = "Hello, World!",
        .content_type = "text/plain",
    };

    char buf[4096];
    int n = hl_smtp_format_message(&msg, buf, (int)sizeof(buf));
    ASSERT_TRUE(n > 0);

    /* Check required headers are present */
    ASSERT_TRUE(strstr(buf, "From: sender@example.com\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "To: recipient@example.com\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Subject: Test Subject\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "MIME-Version: 1.0\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Date: ") != NULL);

    /* Body is present after blank line */
    ASSERT_TRUE(strstr(buf, "\r\n\r\nHello, World!") != NULL);
}

UTEST(format, html_content_type)
{
    HlSmtpMessage msg = {
        .from = "a@b.com",
        .to = "c@d.com",
        .subject = "HTML",
        .body = "<h1>Hi</h1>",
        .content_type = "text/html",
    };

    char buf[4096];
    int n = hl_smtp_format_message(&msg, buf, (int)sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
}

UTEST(format, with_cc)
{
    const char *cc[] = { "cc1@example.com", "cc2@example.com" };
    HlSmtpMessage msg = {
        .from = "a@b.com",
        .to = "c@d.com",
        .cc = cc,
        .cc_count = 2,
        .subject = "CC Test",
        .body = "body",
        .content_type = "text/plain",
    };

    char buf[4096];
    int n = hl_smtp_format_message(&msg, buf, (int)sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "Cc: cc1@example.com, cc2@example.com\r\n") != NULL);
}

UTEST(format, with_reply_to)
{
    HlSmtpMessage msg = {
        .from = "a@b.com",
        .to = "c@d.com",
        .reply_to = "reply@example.com",
        .subject = "Reply-To Test",
        .body = "body",
        .content_type = "text/plain",
    };

    char buf[4096];
    int n = hl_smtp_format_message(&msg, buf, (int)sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "Reply-To: reply@example.com\r\n") != NULL);
}

UTEST(format, dot_stuffing)
{
    HlSmtpMessage msg = {
        .from = "a@b.com",
        .to = "c@d.com",
        .subject = "Dots",
        .body = "line1\n.line2\n..line3",
        .content_type = "text/plain",
    };

    char buf[4096];
    int n = hl_smtp_format_message(&msg, buf, (int)sizeof(buf));
    ASSERT_TRUE(n > 0);

    /* After the blank line separator, body should have dots doubled */
    const char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;  /* skip \r\n\r\n */

    /* .line2 should become ..line2 */
    ASSERT_TRUE(strstr(body_start, "\n..line2\n") != NULL);
    /* ..line3 should become ...line3 */
    ASSERT_TRUE(strstr(body_start, "\n...line3") != NULL);
}

UTEST(format, default_content_type)
{
    HlSmtpMessage msg = {
        .from = "a@b.com",
        .to = "c@d.com",
        .subject = "No CT",
        .body = "body",
        .content_type = NULL,
    };

    char buf[4096];
    int n = hl_smtp_format_message(&msg, buf, (int)sizeof(buf));
    ASSERT_TRUE(n > 0);
    /* Should default to text/plain */
    ASSERT_TRUE(strstr(buf, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);
}

UTEST(format, buffer_too_small)
{
    HlSmtpMessage msg = {
        .from = "a@b.com",
        .to = "c@d.com",
        .subject = "Test",
        .body = "body",
        .content_type = "text/plain",
    };

    char buf[10];  /* way too small */
    int n = hl_smtp_format_message(&msg, buf, (int)sizeof(buf));
    ASSERT_EQ(-1, n);
}

UTEST(format, null_message)
{
    char buf[4096];
    ASSERT_EQ(-1, hl_smtp_format_message(NULL, buf, (int)sizeof(buf)));
}

/* ════════════════════════════════════════════════════════════════════
 * Host allowlist tests
 * ════════════════════════════════════════════════════════════════════ */

UTEST(smtp_host, allowed)
{
    const char *hosts[] = { "smtp.example.com", "smtp.gmail.com" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 2 };
    ASSERT_EQ(0, hl_smtp_check_host(&cfg, "smtp.example.com"));
    ASSERT_EQ(0, hl_smtp_check_host(&cfg, "smtp.gmail.com"));
}

UTEST(smtp_host, denied)
{
    const char *hosts[] = { "smtp.example.com" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 1 };
    ASSERT_NE(0, hl_smtp_check_host(&cfg, "evil.com"));
}

UTEST(smtp_host, case_insensitive)
{
    const char *hosts[] = { "SMTP.Example.COM" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 1 };
    ASSERT_EQ(0, hl_smtp_check_host(&cfg, "smtp.example.com"));
}

UTEST(smtp_host, empty_list)
{
    HlSmtpConfig cfg = { .allowed_hosts = NULL, .host_count = 0 };
    ASSERT_NE(0, hl_smtp_check_host(&cfg, "smtp.example.com"));
}

/* §2.8: smtp shares the http glob/CIDR matcher. */
UTEST(smtp_host, wildcard_subdomain)
{
    const char *hosts[] = { "*.example.com" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 1 };
    ASSERT_EQ(0, hl_smtp_check_host(&cfg, "smtp.example.com"));
    ASSERT_NE(0, hl_smtp_check_host(&cfg, "example.com"));   /* apex */
    ASSERT_NE(0, hl_smtp_check_host(&cfg, "evil.com"));
}

UTEST(smtp_host, null_cfg)
{
    ASSERT_NE(0, hl_smtp_check_host(NULL, "smtp.example.com"));
}

/* ════════════════════════════════════════════════════════════════════
 * CRLF injection guard tests (via hl_cap_smtp_send validation)
 * ════════════════════════════════════════════════════════════════════ */

UTEST(smtp_send, null_cfg)
{
    HlSmtpMessage msg = {
        .host = "smtp.example.com", .port = 587,
        .from = "a@b.com", .to = "c@d.com",
        .subject = "test", .body = "body",
    };
    ASSERT_NE(0, hl_cap_smtp_send(NULL, &msg, NULL));
}

UTEST(smtp_send, null_msg)
{
    const char *hosts[] = { "smtp.example.com" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 1 };
    ASSERT_NE(0, hl_cap_smtp_send(&cfg, NULL, NULL));
}

UTEST(smtp_send, crlf_in_subject)
{
    const char *hosts[] = { "smtp.example.com" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 1 };
    HlSmtpMessage msg = {
        .host = "smtp.example.com", .port = 587,
        .from = "a@b.com", .to = "c@d.com",
        .subject = "evil\r\nBcc: victim@evil.com",
        .body = "body",
    };
    /* Should reject due to CRLF in subject (before any network I/O) */
    ASSERT_NE(0, hl_cap_smtp_send(&cfg, &msg, NULL));
}

UTEST(smtp_send, crlf_in_from)
{
    const char *hosts[] = { "smtp.example.com" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 1 };
    HlSmtpMessage msg = {
        .host = "smtp.example.com", .port = 587,
        .from = "a@b.com\r\nBcc: x@evil.com", .to = "c@d.com",
        .subject = "test", .body = "body",
    };
    ASSERT_NE(0, hl_cap_smtp_send(&cfg, &msg, NULL));
}

UTEST(smtp_send, host_not_allowed)
{
    const char *hosts[] = { "smtp.postmarkapp.com" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 1 };
    HlSmtpMessage msg = {
        .host = "smtp.evil.com", .port = 587,
        .from = "a@b.com", .to = "c@d.com",
        .subject = "test", .body = "body",
    };
    ASSERT_NE(0, hl_cap_smtp_send(&cfg, &msg, NULL));
}

UTEST(smtp_send, missing_required_fields)
{
    const char *hosts[] = { "smtp.example.com" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 1 };

    /* Missing from */
    HlSmtpMessage msg1 = {
        .host = "smtp.example.com", .port = 587,
        .to = "c@d.com", .subject = "test", .body = "body",
    };
    ASSERT_NE(0, hl_cap_smtp_send(&cfg, &msg1, NULL));

    /* Missing to */
    HlSmtpMessage msg2 = {
        .host = "smtp.example.com", .port = 587,
        .from = "a@b.com", .subject = "test", .body = "body",
    };
    ASSERT_NE(0, hl_cap_smtp_send(&cfg, &msg2, NULL));

    /* Missing subject */
    HlSmtpMessage msg3 = {
        .host = "smtp.example.com", .port = 587,
        .from = "a@b.com", .to = "c@d.com", .body = "body",
    };
    ASSERT_NE(0, hl_cap_smtp_send(&cfg, &msg3, NULL));

    /* Missing body */
    HlSmtpMessage msg4 = {
        .host = "smtp.example.com", .port = 587,
        .from = "a@b.com", .to = "c@d.com", .subject = "test",
    };
    ASSERT_NE(0, hl_cap_smtp_send(&cfg, &msg4, NULL));
}

UTEST(smtp_send, invalid_port)
{
    const char *hosts[] = { "smtp.example.com" };
    HlSmtpConfig cfg = { .allowed_hosts = hosts, .host_count = 1 };
    HlSmtpMessage msg = {
        .host = "smtp.example.com", .port = 0,
        .from = "a@b.com", .to = "c@d.com",
        .subject = "test", .body = "body",
    };
    ASSERT_NE(0, hl_cap_smtp_send(&cfg, &msg, NULL));
}

UTEST_MAIN()
