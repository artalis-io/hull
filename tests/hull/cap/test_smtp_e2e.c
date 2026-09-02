/*
 * test_smtp_e2e.c - End-to-end SMTP tests with in-process mock server
 *
 * A mock SMTP server on a background thread accepts one connection,
 * speaks the SMTP protocol, and captures the conversation.  Tests
 * verify the full path: connect → greeting → EHLO → AUTH → MAIL FROM
 * → RCPT TO → DATA → QUIT over real TCP sockets.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/smtp.h"

#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>

/* ════════════════════════════════════════════════════════════════════
 * Mock SMTP server
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    int             listen_fd;
    int             port;
    pthread_t       tid;

    /* Captured from last conversation */
    char            ehlo_host[256];
    char            mail_from[256];
    char            rcpt_to[4][256];
    int             rcpt_count;
    char            auth_plain[512];
    char            data_buf[8192];
    int             data_len;
    int             quit_seen;

    /* Configurable behavior */
    int             reject_rcpt;
    int             require_auth;
    int             withhold_greeting;  /* accept, then never send 220 (Dop test) */
} MockSmtp;

/* Read one CRLF-terminated line from the client */
static int mock_read_line(int fd, char *buf, int size)
{
    int pos = 0;
    while (pos < size - 1) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n <= 0)
            return -1;
        pos++;
        if (pos >= 2 && buf[pos - 2] == '\r' && buf[pos - 1] == '\n') {
            buf[pos] = '\0';
            return pos;
        }
    }
    buf[pos] = '\0';
    return pos;
}

static int mock_send(int fd, const char *s)
{
    size_t len = strlen(s);
    ssize_t w = write(fd, s, len);
    return (w == (ssize_t)len) ? 0 : -1;
}

static void *mock_smtp_thread(void *arg)
{
    MockSmtp *m = (MockSmtp *)arg;

    int client = accept(m->listen_fd, NULL, NULL);
    if (client < 0)
        return NULL;

    /* Dop test: a peer that accepted the TCP connection (so DNS + connect
     * succeeded, i.e. we are POST-resolution) but withholds its 220 greeting, so
     * the greeting-read stage runs until the frozen operation deadline Dop fires. */
    if (m->withhold_greeting) {
        /* Never send 220. Block reading until the client (the transport) closes on
         * its Dop expiry, so this thread exits promptly for a fast join while still
         * having withheld the greeting for the entire pre-Dop window (the transport
         * only reads in the greeting stage, so this read blocks until Dop fires and
         * the abortive teardown closes the fd). A watchdog caps a stuck peer. */
        char buf[64];
        for (int i = 0; i < 400; i++) {         /* <= ~20s watchdog */
            struct timeval tv = { 0, 50000 };   /* 50ms poll for close */
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            ssize_t n = read(client, buf, sizeof buf);
            if (n == 0) break;                  /* transport closed (Dop fired) */
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        }
        close(client);
        return NULL;
    }

    /* 220 greeting */
    mock_send(client, "220 mock.local ESMTP\r\n");

    char line[1024];
    int in_data = 0;

    while (mock_read_line(client, line, (int)sizeof(line)) > 0) {
        if (in_data) {
            /* Data terminator: standalone ".\r\n" */
            if (strcmp(line, ".\r\n") == 0) {
                in_data = 0;
                mock_send(client, "250 OK\r\n");
                continue;
            }
            /* Append to captured data buffer */
            int ll = (int)strlen(line);
            if (m->data_len + ll < (int)sizeof(m->data_buf) - 1) {
                memcpy(m->data_buf + m->data_len, line, (size_t)ll);
                m->data_len += ll;
                m->data_buf[m->data_len] = '\0';
            }
            continue;
        }

        if (strncasecmp(line, "EHLO ", 5) == 0) {
            int len = (int)strlen(line);
            int hlen = len - 5 - 2; /* strip "EHLO " prefix and "\r\n" */
            if (hlen > 0 && hlen < (int)sizeof(m->ehlo_host)) {
                memcpy(m->ehlo_host, line + 5, (size_t)hlen);
                m->ehlo_host[hlen] = '\0';
            }
            if (m->require_auth)
                mock_send(client,
                          "250-mock.local\r\n250 AUTH PLAIN\r\n");
            else
                mock_send(client, "250 mock.local\r\n");
        }
        else if (strncasecmp(line, "AUTH PLAIN ", 11) == 0) {
            int len = (int)strlen(line);
            int clen = len - 11 - 2; /* strip prefix and "\r\n" */
            if (clen > 0 && clen < (int)sizeof(m->auth_plain)) {
                memcpy(m->auth_plain, line + 11, (size_t)clen);
                m->auth_plain[clen] = '\0';
            }
            mock_send(client, "235 OK\r\n");
        }
        else if (strncasecmp(line, "MAIL FROM:", 10) == 0) {
            const char *lt = strchr(line, '<');
            const char *gt = strchr(line, '>');
            if (lt && gt && gt > lt) {
                int alen = (int)(gt - lt - 1);
                if (alen > 0 && alen < (int)sizeof(m->mail_from)) {
                    memcpy(m->mail_from, lt + 1, (size_t)alen);
                    m->mail_from[alen] = '\0';
                }
            }
            mock_send(client, "250 OK\r\n");
        }
        else if (strncasecmp(line, "RCPT TO:", 8) == 0) {
            if (m->reject_rcpt) {
                mock_send(client, "550 Rejected\r\n");
            } else {
                const char *lt = strchr(line, '<');
                const char *gt = strchr(line, '>');
                if (lt && gt && gt > lt && m->rcpt_count < 4) {
                    int alen = (int)(gt - lt - 1);
                    if (alen > 0 && alen < (int)sizeof(m->rcpt_to[0])) {
                        memcpy(m->rcpt_to[m->rcpt_count], lt + 1,
                               (size_t)alen);
                        m->rcpt_to[m->rcpt_count][alen] = '\0';
                        m->rcpt_count++;
                    }
                }
                mock_send(client, "250 OK\r\n");
            }
        }
        else if (strncasecmp(line, "DATA", 4) == 0) {
            in_data = 1;
            mock_send(client, "354 Go\r\n");
        }
        else if (strncasecmp(line, "QUIT", 4) == 0) {
            m->quit_seen = 1;
            mock_send(client, "221 Bye\r\n");
            break;
        }
    }

    close(client);
    return NULL;
}

static int mock_smtp_start(MockSmtp *m)
{
    memset(m, 0, sizeof(*m));
    m->listen_fd = -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; /* ephemeral */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    socklen_t slen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &slen) < 0) {
        close(fd);
        return -1;
    }
    m->port = ntohs(addr.sin_port);

    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }

    m->listen_fd = fd;
    if (pthread_create(&m->tid, NULL, mock_smtp_thread, m) != 0) {
        close(fd);
        m->listen_fd = -1;
        return -1;
    }

    return 0;
}

static void mock_smtp_stop(MockSmtp *m)
{
    /* Close listen fd to unblock accept() if no client connected */
    if (m->listen_fd >= 0) {
        close(m->listen_fd);
        m->listen_fd = -1;
    }
    pthread_join(m->tid, NULL);
}

/* ════════════════════════════════════════════════════════════════════
 * Tests
 * ════════════════════════════════════════════════════════════════════ */

#define TEST_TIMEOUT_MS 2000

/* ── 1. Basic plain send ─────────────────────────────────────────── */

UTEST(smtp_e2e, plain_send)
{
    MockSmtp m;
    ASSERT_EQ(0, mock_smtp_start(&m));

    const char *hosts[] = { "127.0.0.1" };
    HlSmtpConfig cfg = {
        .allowed_hosts = hosts,
        .host_count    = 1,
        .timeout_ms    = TEST_TIMEOUT_MS,
    };

    HlSmtpMessage msg = {
        .host         = "127.0.0.1",
        .port         = m.port,
        .use_tls      = 0,
        .from         = "sender@example.com",
        .to           = "recipient@example.com",
        .subject      = "Hello",
        .body         = "Hello, World!",
        .content_type = "text/plain",
    };

    ASSERT_EQ(0, hl_cap_smtp_send(&cfg, &msg, NULL));
    mock_smtp_stop(&m);

    /* EHLO hostname */
    ASSERT_EQ(0, strcmp(m.ehlo_host, "localhost"));

    /* Envelope addresses */
    ASSERT_EQ(0, strcmp(m.mail_from, "sender@example.com"));
    ASSERT_EQ(1, m.rcpt_count);
    ASSERT_EQ(0, strcmp(m.rcpt_to[0], "recipient@example.com"));

    /* QUIT seen */
    ASSERT_TRUE(m.quit_seen);

    /* DATA payload contains expected headers and body */
    ASSERT_TRUE(strstr(m.data_buf, "From: sender@example.com\r\n") != NULL);
    ASSERT_TRUE(strstr(m.data_buf, "To: recipient@example.com\r\n") != NULL);
    ASSERT_TRUE(strstr(m.data_buf, "Subject: Hello\r\n") != NULL);
    ASSERT_TRUE(strstr(m.data_buf, "Content-Type: text/plain") != NULL);
    ASSERT_TRUE(strstr(m.data_buf, "Hello, World!") != NULL);
}

static long elapsed_ms(struct timespec *a, struct timespec *b)
{ return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000; }

/* FROZEN post-resolution operation deadline (Dop, section 8), GREETING stage: a
 * peer accepts the connection (post-resolution) but withholds its 220 greeting, so
 * the greeting read runs until Dop. Public token stays connect_failed;
 * deadline_expired is set (audit adds terminal:post_resolution_deadline).
 * REVERT PROOF: remove the Dop classification and the greeting read ends on its
 * stage budget with token "greeting_failed" and deadline_expired 0 - both asserts
 * below flip. The peer holds until the transport closes, so only Dop ends it. */
UTEST(smtp_e2e, post_resolution_deadline_greeting_stage)
{
    MockSmtp m;
    ASSERT_EQ(0, mock_smtp_start(&m));
    m.withhold_greeting = 1;

    HlSmtpMessage msg = {
        .host = "127.0.0.1", .port = m.port, .use_tls = 0,
        .from = "s@example.com", .to = "r@example.com",
        .subject = "hi", .body = "b", .content_type = "text/plain",
    };

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    HlSmtpResult r;
    hl_smtp_execute(&msg, NULL, 300, NULL, NULL, &r);   /* Dop = 300ms */
    clock_gettime(CLOCK_MONOTONIC, &t1);

    ASSERT_EQ(r.rc, -1);
    ASSERT_STREQ(r.token, "connect_failed");    /* revert -> "greeting_failed" */
    ASSERT_EQ(r.deadline_expired, 1);            /* revert -> 0 */
    ASSERT_TRUE(elapsed_ms(&t0, &t1) < 1500);    /* ~Dop; watchdog vs an unbounded read */
    mock_smtp_stop(&m);
}

/* The pending-connect Dop and the deadline-versus-cancel precedence are covered
 * deterministically (injected resolver + fault-injection socket provider, no
 * environmental skip) in tests/hull/cap/test_smtp_transport.c:
 *   - pending_connect_dop_expires_deterministically
 *   - precedence_dop_beats_cancel_at_one_checkpoint / _live_pump
 * The earlier TEST-NET-1 blackhole probe (environment-skippable) and the
 * before-DNS cancel-window race were superseded by those and removed. The
 * greeting-stage Dop above stays as the live end-to-end variant. */

/* ── 2. AUTH PLAIN rejected without TLS ──────────────────────────── */

UTEST(smtp_e2e, auth_plain_rejected_without_tls)
{
    MockSmtp m;
    ASSERT_EQ(0, mock_smtp_start(&m));
    m.require_auth = 1;

    const char *hosts[] = { "127.0.0.1" };
    HlSmtpConfig cfg = {
        .allowed_hosts = hosts,
        .host_count    = 1,
        .timeout_ms    = TEST_TIMEOUT_MS,
    };

    HlSmtpMessage msg = {
        .host         = "127.0.0.1",
        .port         = m.port,
        .use_tls      = 0,
        .username     = "user",
        .password     = "pass",
        .from         = "a@b.com",
        .to           = "c@d.com",
        .subject      = "Auth Test",
        .body         = "body",
        .content_type = "text/plain",
    };

    /* AUTH PLAIN must be rejected when TLS is not active */
    ASSERT_EQ(-1, hl_cap_smtp_send(&cfg, &msg, NULL));
    mock_smtp_stop(&m);

    /* Credentials should NOT have been sent */
    ASSERT_EQ(0, strcmp(m.auth_plain, ""));
}

/* ── 3. CC recipients ────────────────────────────────────────────── */

UTEST(smtp_e2e, cc_recipients)
{
    MockSmtp m;
    ASSERT_EQ(0, mock_smtp_start(&m));

    const char *hosts[] = { "127.0.0.1" };
    HlSmtpConfig cfg = {
        .allowed_hosts = hosts,
        .host_count    = 1,
        .timeout_ms    = TEST_TIMEOUT_MS,
    };

    const char *cc[] = { "cc1@example.com", "cc2@example.com" };
    HlSmtpMessage msg = {
        .host         = "127.0.0.1",
        .port         = m.port,
        .use_tls      = 0,
        .from         = "sender@example.com",
        .to           = "primary@example.com",
        .cc           = cc,
        .cc_count     = 2,
        .subject      = "CC Test",
        .body         = "body",
        .content_type = "text/plain",
    };

    ASSERT_EQ(0, hl_cap_smtp_send(&cfg, &msg, NULL));
    mock_smtp_stop(&m);

    /* 1 primary + 2 CC = 3 RCPT TO commands */
    ASSERT_EQ(3, m.rcpt_count);
    ASSERT_EQ(0, strcmp(m.rcpt_to[0], "primary@example.com"));
    ASSERT_EQ(0, strcmp(m.rcpt_to[1], "cc1@example.com"));
    ASSERT_EQ(0, strcmp(m.rcpt_to[2], "cc2@example.com"));

    /* Cc header in DATA payload */
    ASSERT_TRUE(strstr(m.data_buf,
                       "Cc: cc1@example.com, cc2@example.com\r\n") != NULL);
    ASSERT_TRUE(m.quit_seen);
}

/* ── 4. Reply-To + HTML content type ─────────────────────────────── */

UTEST(smtp_e2e, reply_to_and_html)
{
    MockSmtp m;
    ASSERT_EQ(0, mock_smtp_start(&m));

    const char *hosts[] = { "127.0.0.1" };
    HlSmtpConfig cfg = {
        .allowed_hosts = hosts,
        .host_count    = 1,
        .timeout_ms    = TEST_TIMEOUT_MS,
    };

    HlSmtpMessage msg = {
        .host         = "127.0.0.1",
        .port         = m.port,
        .use_tls      = 0,
        .from         = "a@b.com",
        .to           = "c@d.com",
        .reply_to     = "reply@example.com",
        .subject      = "HTML Test",
        .body         = "<h1>Hello</h1>",
        .content_type = "text/html",
    };

    ASSERT_EQ(0, hl_cap_smtp_send(&cfg, &msg, NULL));
    mock_smtp_stop(&m);

    ASSERT_TRUE(strstr(m.data_buf,
                       "Reply-To: reply@example.com\r\n") != NULL);
    ASSERT_TRUE(strstr(m.data_buf,
                       "Content-Type: text/html; charset=utf-8\r\n") != NULL);
    ASSERT_TRUE(strstr(m.data_buf, "<h1>Hello</h1>") != NULL);
    ASSERT_TRUE(m.quit_seen);
}

/* ── 5. Dot-stuffing ─────────────────────────────────────────────── */

UTEST(smtp_e2e, dot_stuffing)
{
    MockSmtp m;
    ASSERT_EQ(0, mock_smtp_start(&m));

    const char *hosts[] = { "127.0.0.1" };
    HlSmtpConfig cfg = {
        .allowed_hosts = hosts,
        .host_count    = 1,
        .timeout_ms    = TEST_TIMEOUT_MS,
    };

    HlSmtpMessage msg = {
        .host         = "127.0.0.1",
        .port         = m.port,
        .use_tls      = 0,
        .from         = "a@b.com",
        .to           = "c@d.com",
        .subject      = "Dots",
        .body         = "line1\r\n.line2\r\n..line3\r\n",
        .content_type = "text/plain",
    };

    ASSERT_EQ(0, hl_cap_smtp_send(&cfg, &msg, NULL));
    mock_smtp_stop(&m);

    /* Find body after header/body separator */
    const char *body = strstr(m.data_buf, "\r\n\r\n");
    ASSERT_TRUE(body != NULL);
    body += 4;

    /* .line2 → ..line2, ..line3 → ...line3 (RFC 5321 §4.5.2) */
    ASSERT_TRUE(strstr(body, "\r\n..line2\r\n") != NULL);
    ASSERT_TRUE(strstr(body, "\r\n...line3\r\n") != NULL);
    ASSERT_TRUE(m.quit_seen);
}

/* ── 6. Connection refused ───────────────────────────────────────── */

UTEST(smtp_e2e, connection_refused)
{
    /* Bind to get an ephemeral port, then close - nothing listening */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    ASSERT_EQ(0, bind(fd, (struct sockaddr *)&addr, sizeof(addr)));

    socklen_t slen = sizeof(addr);
    ASSERT_EQ(0, getsockname(fd, (struct sockaddr *)&addr, &slen));
    int port = ntohs(addr.sin_port);
    close(fd);

    const char *hosts[] = { "127.0.0.1" };
    HlSmtpConfig cfg = {
        .allowed_hosts = hosts,
        .host_count    = 1,
        .timeout_ms    = TEST_TIMEOUT_MS,
    };

    HlSmtpMessage msg = {
        .host         = "127.0.0.1",
        .port         = port,
        .use_tls      = 0,
        .from         = "a@b.com",
        .to           = "c@d.com",
        .subject      = "test",
        .body         = "body",
        .content_type = "text/plain",
    };

    ASSERT_EQ(-1, hl_cap_smtp_send(&cfg, &msg, NULL));
}

/* ── 7. RCPT TO rejected (550) ───────────────────────────────────── */

UTEST(smtp_e2e, rcpt_rejected)
{
    MockSmtp m;
    ASSERT_EQ(0, mock_smtp_start(&m));
    m.reject_rcpt = 1;

    const char *hosts[] = { "127.0.0.1" };
    HlSmtpConfig cfg = {
        .allowed_hosts = hosts,
        .host_count    = 1,
        .timeout_ms    = TEST_TIMEOUT_MS,
    };

    HlSmtpMessage msg = {
        .host         = "127.0.0.1",
        .port         = m.port,
        .use_tls      = 0,
        .from         = "a@b.com",
        .to           = "c@d.com",
        .subject      = "test",
        .body         = "body",
        .content_type = "text/plain",
    };

    ASSERT_EQ(-1, hl_cap_smtp_send(&cfg, &msg, NULL));
    mock_smtp_stop(&m);
}

/* ── 8. Host not in allowlist ────────────────────────────────────── */

UTEST(smtp_e2e, host_not_allowed)
{
    /* Fails at allowlist check - no TCP connection made */
    const char *hosts[] = { "smtp.allowed.com" };
    HlSmtpConfig cfg = {
        .allowed_hosts = hosts,
        .host_count    = 1,
        .timeout_ms    = TEST_TIMEOUT_MS,
    };

    HlSmtpMessage msg = {
        .host         = "127.0.0.1",
        .port         = 25,
        .use_tls      = 0,
        .from         = "a@b.com",
        .to           = "c@d.com",
        .subject      = "test",
        .body         = "body",
        .content_type = "text/plain",
    };

    ASSERT_EQ(-1, hl_cap_smtp_send(&cfg, &msg, NULL));
}

UTEST_MAIN();
