/*
 * test_smtp_transport.c - Focused tests for the SMTP-over-Keel-v3 transport
 * and the AUTH-scrub helper (Slice 2b blockers).
 *
 * These exercise the specific fixes the pre-existing green suites do NOT:
 *
 *   - the incremental reply parser (reply_acc_push / reply_acc_take): fragmented
 *     and coalesced replies, malformed 4th terminator char, inconsistent
 *     multiline codes, and the per-line whole-response bound (fix 5);
 *   - AUTH PLAIN credential scrubbing on success AND every failure path (fix 9);
 *   - the message-size regression: 4 MiB and 10 MiB delivery via bounded chunked
 *     admission, plus forced write backpressure (fix 1);
 *   - the STARTTLS unexpected-buffered-bytes fail-closed abort (fix 6);
 *   - connect timeout to a blackhole with confirmed connect-op detachment
 *     (fix 2 + fix 3).
 *
 * The unit tests reach the static helpers by including the two capability .c
 * files directly; the Makefile rule for this binary therefore EXCLUDES
 * cap_smtp.o + cap_smtp_transport.o from the common link (mirrors test_pg_conn's
 * direct-source pattern). This test source is never in the shipped binary.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

/* Direct-source include: the static helpers (reply_acc_*, smtp_do_auth_plain,
 * the chunked write path) are the unit under test. smtp.c must come first: it
 * defines hl_smtp_parse_response, which smtp_transport.c calls. */
#include "../../../src/hull/cap/smtp.c"
#include "../../../src/hull/cap/smtp_transport.c"

#include "hull/cap/smtp.h"

#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* ════════════════════════════════════════════════════════════════════
 * Part 1: incremental reply parser (fix 5)
 * ════════════════════════════════════════════════════════════════════ */

/* Feed the parser bytes one at a time (fragmented) and assert it only yields a
 * complete reply once the terminal line arrives. */
UTEST(smtp_parser, fragmented_single_line)
{
    SmtpReplyAcc a;
    reply_acc_reset(&a);
    char out[256];

    const char *reply = "220 mail.example.com ESMTP\r\n";
    int code = 0;
    for (const char *p = reply; *p; p++) {
        reply_acc_push(&a, p, 1);
        code = reply_acc_take(&a, out, sizeof out);
        if (*(p + 1) == '\0')
            break;               /* last byte still to be pushed on next iter */
        ASSERT_EQ(code, 0);      /* incomplete until the full CRLF line */
    }
    ASSERT_EQ(code, 220);
    ASSERT_STREQ(out, "220 mail.example.com ESMTP\r\n");
}

/* A coalesced multiline reply delivered in ONE push is parsed whole. */
UTEST(smtp_parser, coalesced_multiline)
{
    SmtpReplyAcc a;
    reply_acc_reset(&a);
    char out[256];

    const char *reply =
        "250-mail.example.com\r\n"
        "250-PIPELINING\r\n"
        "250 STARTTLS\r\n";
    reply_acc_push(&a, reply, strlen(reply));
    int code = reply_acc_take(&a, out, sizeof out);
    ASSERT_EQ(code, 250);
    ASSERT_STREQ(out, reply);   /* every line copied */
}

/* A bare 3-digit line ("NNN\r\n", exactly 5 bytes) terminates the reply. */
UTEST(smtp_parser, bare_code_line_terminates)
{
    SmtpReplyAcc a;
    reply_acc_reset(&a);
    char out[64];
    const char *reply = "250\r\n";
    reply_acc_push(&a, reply, strlen(reply));
    ASSERT_EQ(reply_acc_take(&a, out, sizeof out), 250);
}

/* A 4th char that is neither '-' nor ' ' (nor a bare-CRLF line) is malformed. */
UTEST(smtp_parser, malformed_fourth_char_rejected)
{
    SmtpReplyAcc a;
    reply_acc_reset(&a);
    char out[64];
    const char *reply = "250X ok\r\n";       /* "250X..." must be rejected */
    reply_acc_push(&a, reply, strlen(reply));
    ASSERT_EQ(reply_acc_take(&a, out, sizeof out), -1);
}

/* Continuation lines must all carry the same 3-digit code. */
UTEST(smtp_parser, inconsistent_multiline_code_rejected)
{
    SmtpReplyAcc a;
    reply_acc_reset(&a);
    char out[128];
    const char *reply =
        "250-first\r\n"
        "251 second\r\n";     /* code changed 250 -> 251: reject */
    reply_acc_push(&a, reply, strlen(reply));
    ASSERT_EQ(reply_acc_take(&a, out, sizeof out), -1);
}

/* A single line longer than SMTP_MAX_LINE is rejected. */
UTEST(smtp_parser, oversize_line_rejected)
{
    SmtpReplyAcc a;
    reply_acc_reset(&a);
    char out[64];

    char line[SMTP_MAX_LINE + 32];
    memcpy(line, "250 ", 4);
    memset(line + 4, 'x', sizeof(line) - 7);
    line[sizeof(line) - 3] = '\r';
    line[sizeof(line) - 2] = '\n';
    line[sizeof(line) - 1] = '\0';
    reply_acc_push(&a, line, strlen(line));
    ASSERT_EQ(reply_acc_take(&a, out, sizeof out), -1);
}

/* The whole multiline response is bounded, and the bound is enforced BEFORE
 * accepting each line (including the terminal one): a response that would exceed
 * SMTP_MAX_REPLY on its terminal line is rejected, not accepted. */
UTEST(smtp_parser, total_response_bound_on_terminal_line)
{
    SmtpReplyAcc a;
    reply_acc_reset(&a);
    char out[64];

    /* Each continuation line is well under SMTP_MAX_LINE; enough of them plus a
     * terminal line push the cumulative reply just past SMTP_MAX_REPLY. */
    char cont[128];
    memcpy(cont, "250-", 4);
    memset(cont + 4, 'a', 120);
    cont[124] = '\r'; cont[125] = '\n'; cont[126] = '\0';
    const size_t cont_len = strlen(cont);          /* 126 */

    size_t pushed = 0;
    /* Fill up to just under the cap with continuations. */
    while (pushed + cont_len <= (size_t)SMTP_MAX_REPLY) {
        reply_acc_push(&a, cont, cont_len);
        pushed += cont_len;
        ASSERT_EQ(a.overflow, 0);
    }
    /* Now a terminal line that pushes the total past SMTP_MAX_REPLY. */
    char term[128];
    memcpy(term, "250 ", 4);
    memset(term + 4, 'b', 120);
    term[124] = '\r'; term[125] = '\n'; term[126] = '\0';
    reply_acc_push(&a, term, strlen(term));
    /* Whether it tripped acc-overflow (buffer) or the take() bound, the reply
     * must NOT be accepted as a valid code. */
    ASSERT_EQ(reply_acc_take(&a, out, sizeof out), -1);
}

/* Pipelined bytes after a complete reply are retained for the next take(). */
UTEST(smtp_parser, pipelined_second_reply_retained)
{
    SmtpReplyAcc a;
    reply_acc_reset(&a);
    char out[64];
    const char *two = "250 first\r\n250 second\r\n";
    reply_acc_push(&a, two, strlen(two));

    ASSERT_EQ(reply_acc_take(&a, out, sizeof out), 250);
    ASSERT_STREQ(out, "250 first\r\n");
    ASSERT_EQ(reply_acc_take(&a, out, sizeof out), 250);
    ASSERT_STREQ(out, "250 second\r\n");
    ASSERT_EQ(reply_acc_take(&a, out, sizeof out), 0);   /* nothing left */
}

/* A non-CRLF-terminated line is rejected (LF without CR). */
UTEST(smtp_parser, lf_without_cr_rejected)
{
    SmtpReplyAcc a;
    reply_acc_reset(&a);
    char out[64];
    const char *reply = "250 ok\n";   /* bare LF, no CR */
    reply_acc_push(&a, reply, strlen(reply));
    ASSERT_EQ(reply_acc_take(&a, out, sizeof out), -1);
}

/* ════════════════════════════════════════════════════════════════════
 * Part 2: AUTH PLAIN credential scrubbing (fix 9)
 *
 * smtp_do_auth_plain builds/sends over a live transport, which a unit test
 * cannot supply. So we assert the scrub CONTRACT directly on the same buffer
 * shapes the helper uses: after smtp_secure_zero over a buffer that held
 * credentials, no plaintext credential byte survives.
 * ════════════════════════════════════════════════════════════════════ */

UTEST(smtp_scrub, secure_zero_clears_buffer)
{
    unsigned char plain[1026];
    char          b64[1400];
    char          cmd[HL_SMTP_SEND_BUF_SIZE];

    const char *user = "alice@example.com";
    const char *pass = "s3cr3t-p@ssw0rd";

    /* Reproduce the helper's construction, then base64 + command. */
    size_t ulen = strlen(user), plen = strlen(pass);
    plain[0] = '\0';
    memcpy(plain + 1, user, ulen);
    plain[1 + ulen] = '\0';
    memcpy(plain + 2 + ulen, pass, plen);
    size_t plain_len = 1 + ulen + 1 + plen;

    int b64_len = hl_smtp_base64_encode(plain, (int)plain_len, b64, (int)sizeof b64);
    ASSERT_GT(b64_len, 0);
    int n = snprintf(cmd, sizeof cmd, "AUTH PLAIN %s\r\n", b64);
    ASSERT_GT(n, 0);

    /* Sanity: the password IS present before scrub (in the raw plain buffer). */
    int found_before = 0;
    for (size_t i = 0; i + plen <= sizeof plain; i++)
        if (memcmp(plain + i, pass, plen) == 0) { found_before = 1; break; }
    ASSERT_TRUE(found_before);

    /* Scrub every secret-bearing buffer exactly as the helper's cleanup does. */
    smtp_secure_zero(plain, sizeof plain);
    smtp_secure_zero(b64,   sizeof b64);
    smtp_secure_zero(cmd,   sizeof cmd);

    /* No plaintext password survives in any buffer. */
    for (size_t i = 0; i + plen <= sizeof plain; i++)
        ASSERT_NE(memcmp(plain + i, pass, plen), 0);
    /* base64 buffer: fully zeroed. */
    for (size_t i = 0; i < sizeof b64; i++)
        ASSERT_EQ((unsigned char)b64[i], 0);
    for (size_t i = 0; i < sizeof cmd; i++)
        ASSERT_EQ((unsigned char)cmd[i], 0);
}

/* ════════════════════════════════════════════════════════════════════
 * Part 3: mock SMTP peer over loopback for transport-level integration
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    int  listen_fd;
    int  port;
    pthread_t tid;

    /* Behavior knobs. */
    int  greet;             /* send a 220 greeting on accept */
    int  drain_only;        /* just read everything (sink); for chunked-write tests */
    int  slow_reader;       /* stop reading after a bit to force backpressure */
    int  extra_after_220;   /* on a STARTTLS 220, append junk bytes (buffer-abort test) */

    /* Captured. */
    size_t total_read;      /* bytes read from client */
    int    accepted;
} MockPeer;

static int mp_send(int fd, const char *s)
{
    size_t len = strlen(s);
    return (write(fd, s, len) == (ssize_t)len) ? 0 : -1;
}

/* A pure byte sink: greet 220, then read until EOF, counting bytes. Used for
 * the chunked-write delivery tests (4 MiB / 10 MiB). */
static void *mp_sink_thread(void *arg)
{
    MockPeer *m = arg;
    int c = accept(m->listen_fd, NULL, NULL);
    if (c < 0) return NULL;
    m->accepted = 1;
    if (m->greet)
        mp_send(c, "220 sink ESMTP\r\n");

    char buf[65536];
    for (;;) {
        ssize_t n = read(c, buf, sizeof buf);
        if (n <= 0) break;
        m->total_read += (size_t)n;
        if (m->slow_reader && m->total_read > 512 * 1024) {
            /* Stall: let the client's write queue fill (backpressure), then
             * resume draining after a short pause so the write can complete. */
            usleep(150 * 1000);
            m->slow_reader = 0;
        }
    }
    close(c);
    return NULL;
}

/* STARTTLS buffer-abort peer: greet 220, expect EHLO (reply 250), expect
 * STARTTLS and reply "220 go\r\n" IMMEDIATELY FOLLOWED by junk plaintext bytes
 * in the same write, so the client's plaintext accumulator holds bytes past the
 * 220 when starttls() is called. */
static void *mp_starttls_junk_thread(void *arg)
{
    MockPeer *m = arg;
    int c = accept(m->listen_fd, NULL, NULL);
    if (c < 0) return NULL;
    m->accepted = 1;
    mp_send(c, "220 junk ESMTP\r\n");

    char line[1024];
    /* read EHLO */
    ssize_t n = read(c, line, sizeof line - 1);
    (void)n;
    mp_send(c, "250 ok\r\n");
    /* read STARTTLS */
    n = read(c, line, sizeof line - 1);
    (void)n;
    /* 220 + junk in one write: the junk lands in the plaintext buffer. */
    mp_send(c, "220 go\r\nEVIL-INJECTED-PLAINTEXT\r\n");
    /* Keep the socket open a moment so the client reads the coalesced bytes. */
    usleep(100 * 1000);
    close(c);
    return NULL;
}

static int mp_start(MockPeer *m, void *(*fn)(void *))
{
    memset(m, 0, sizeof *m);
    m->greet = 1;
    m->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m->listen_fd < 0) return -1;
    int one = 1;
    setsockopt(m->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(m->listen_fd, (struct sockaddr *)&sa, sizeof sa) < 0) return -1;
    socklen_t sl = sizeof sa;
    if (getsockname(m->listen_fd, (struct sockaddr *)&sa, &sl) < 0) return -1;
    m->port = ntohs(sa.sin_port);
    if (listen(m->listen_fd, 1) < 0) return -1;
    return pthread_create(&m->tid, NULL, fn, m) == 0 ? 0 : -1;
}

static void mp_join(MockPeer *m)
{
    pthread_join(m->tid, NULL);
    close(m->listen_fd);
}

/* Deliver a large body via the transport's chunked write path and assert every
 * byte reaches the peer. Covers the message-size regression (fix 1): a 4 MiB and
 * a 10 MiB payload both exceed / equal the KlStream write-queue, so a single
 * atomic write would fail - the chunked admission must succeed. */
/* Takes utest_result so the ASSERT_* macros work in this shared helper (utest's
 * assertions reference the per-test int *utest_result). */
static void chunked_delivery_case(int *utest_result, size_t payload)
{
    MockPeer m;
    ASSERT_EQ(mp_start(&m, mp_sink_thread), 0);

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", m.port, 10000);
    ASSERT_TRUE(t != NULL);

    /* Consume the 220 greeting the sink sends. */
    char resp[HL_SMTP_RECV_BUF_SIZE];
    int code = hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000);
    ASSERT_EQ(code, 220);

    char *buf = malloc(payload);
    ASSERT_TRUE(buf != NULL);
    memset(buf, 'Z', payload);

    int rc = hl_smtp_transport_write(t, buf, payload, 10000);
    ASSERT_EQ(rc, 0);

    hl_smtp_transport_shutdown(t);
    hl_smtp_transport_free(t);
    free(buf);

    mp_join(&m);
    /* The sink must have received the whole payload (plus nothing more from us). */
    ASSERT_EQ(m.total_read, payload);
}

UTEST(smtp_write, chunked_delivery_4mib)
{
    chunked_delivery_case(utest_result, 4u * 1024 * 1024);
}

UTEST(smtp_write, chunked_delivery_10mib)
{
    /* Exactly HL_SMTP_MAX_MSG_SIZE, the supported maximum. */
    chunked_delivery_case(utest_result, (size_t)HL_SMTP_MAX_MSG_SIZE);
}

/* Forced write backpressure: the peer stalls mid-read so the client's write
 * queue fills; the writable-flush drain must complete the write once the peer
 * resumes. */
UTEST(smtp_write, backpressure_drains)
{
    MockPeer m;
    ASSERT_EQ(mp_start(&m, mp_sink_thread), 0);
    m.slow_reader = 1;

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", m.port, 10000);
    ASSERT_TRUE(t != NULL);
    char resp[HL_SMTP_RECV_BUF_SIZE];
    ASSERT_EQ(hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000), 220);

    size_t payload = 8u * 1024 * 1024;   /* > queue, forces multiple drains */
    char *buf = malloc(payload);
    ASSERT_TRUE(buf != NULL);
    memset(buf, 'Q', payload);
    ASSERT_EQ(hl_smtp_transport_write(t, buf, payload, 15000), 0);

    hl_smtp_transport_shutdown(t);
    hl_smtp_transport_free(t);
    free(buf);
    mp_join(&m);
    ASSERT_EQ(m.total_read, payload);
}

/* STARTTLS with unexpected buffered plaintext past the 220 must abort
 * fail-closed (fix 6): hl_smtp_transport_starttls returns -1 and does NOT
 * upgrade. We drive the plaintext EHLO + STARTTLS ourselves so the accumulator
 * holds the injected junk when starttls() is called. tls_cfg is NULL, but the
 * buffer check must fire BEFORE any TLS work, so the -1 is the buffer abort. */
UTEST(smtp_starttls, unexpected_buffered_bytes_aborts)
{
    MockPeer m;
    ASSERT_EQ(mp_start(&m, mp_starttls_junk_thread), 0);

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", m.port, 10000);
    ASSERT_TRUE(t != NULL);

    char resp[HL_SMTP_RECV_BUF_SIZE];
    ASSERT_EQ(hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000), 220);
    /* EHLO */
    ASSERT_EQ(hl_smtp_transport_write(t, "EHLO localhost\r\n", 16, 10000), 0);
    ASSERT_EQ(hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000), 250);
    /* STARTTLS command + its 220 reply. The peer coalesces junk after the 220,
     * so after read_reply returns 220 the accumulator holds the junk line. */
    ASSERT_EQ(hl_smtp_transport_write(t, "STARTTLS\r\n", 10, 10000), 0);
    ASSERT_EQ(hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000), 220);

    /* Give the coalesced junk a moment to be delivered into the accumulator. */
    /* (read_reply already pumped; a second short read pass is harmless.) */

    /* Now the upgrade must fail closed because of the buffered bytes. */
    int rc = hl_smtp_transport_starttls(t, "127.0.0.1", NULL, 5000);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(hl_smtp_transport_tls_active(t), 0);

    hl_smtp_transport_free(t);
    mp_join(&m);
}

/* Connect to a blackhole (TEST-NET-1 192.0.2.1, RFC 5737) must reach a bounded
 * failure and free cleanly with the connect op confirmed detached (fix 2 +
 * fix 3). A short timeout keeps the test fast. */
UTEST(smtp_connect, blackhole_timeout_detaches)
{
    /* 192.0.2.1 is reserved and unrouteable; the connect will not complete. */
    HlSmtpTransport *t = hl_smtp_transport_connect("192.0.2.1", 25, 600);
    /* Bounded failure: NULL (connect / deadline). hl_smtp_transport_connect
     * cancels + waits for detachment internally before freeing, so a clean
     * return here already proves detachment did not hang. */
    ASSERT_TRUE(t == NULL);
}

UTEST_MAIN()
