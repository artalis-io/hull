/*
 * test_smtp_transport.c - Focused tests for the SMTP-over-Keel-v3 transport
 * and the AUTH-scrub helper (transport review blockers).
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

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <keel/tls.h>   /* KlTls / KlTlsConfig: mock the TLS session seam */

/* ════════════════════════════════════════════════════════════════════
 * Mock KlTls infrastructure (for the STARTTLS vtable-rejection tests).
 *
 * mock_tls_factory returns g_mock_tls (a KlTls with a configurable vtable) and
 * bumps g_mock_tls_factory_calls, so a test can prove starttls() aborted BEFORE
 * ever reaching the factory (the buffered-bytes boundary) versus reaching it and
 * rejecting a malformed vtable / a bad set_hostname. The required ops are trivial
 * stubs. Three knobs, reset at the top of each test that uses them:
 *   g_mock_destroy_null      - when set, the vtable's destroy is NULL (INVALID
 *                              vtable: kl_tls_vtable_valid fails), so the reject
 *                              path must NULL-check destroy or crash.
 *   g_mock_set_hostname_mode - 0: a working set_hostname returning 0;
 *                              1: set_hostname absent (NULL hook);
 *                              2: set_hostname present but returns -1.
 *   g_mock_handshake_result  - the KlTlsResult the mock handshake returns
 *                              (defaults to KL_TLS_OK).
 * ════════════════════════════════════════════════════════════════════ */

static int         g_mock_tls_factory_calls;
static int         g_mock_destroy_null;
static int         g_mock_set_hostname_mode;
static KlTlsResult g_mock_handshake_result;

static KlTlsResult mock_tls_handshake(KlTls *self, KlSocketHandle fd)
{ (void)self; (void)fd; return g_mock_handshake_result; }
static kl_ssize_t mock_tls_read(KlTls *self, KlSocketHandle fd, void *b, size_t n)
{ (void)self; (void)fd; (void)b; (void)n; return 0; }
static kl_ssize_t mock_tls_write(KlTls *self, KlSocketHandle fd, const void *b, size_t n)
{ (void)self; (void)fd; (void)b; (void)n; return 0; }
static KlTlsResult mock_tls_shutdown(KlTls *self, KlSocketHandle fd)
{ (void)self; (void)fd; return KL_TLS_OK; }
static size_t mock_tls_pending(KlTls *self) { (void)self; return 0; }
static void mock_tls_reset(KlTls *self) { (void)self; }
static void mock_tls_destroy(KlTls *self) { (void)self; }
static int mock_tls_set_hostname(KlTls *self, const char *host)
{ (void)self; (void)host; return 0; }
static int mock_tls_set_hostname_fail(KlTls *self, const char *host)
{ (void)self; (void)host; return -1; }

static KlTls g_mock_tls;

/* Build g_mock_tls's vtable from the current knobs and return it. */
static KlTls *mock_tls_factory(KlTlsCtx *ctx, KlAllocator *alloc)
{
    (void)ctx; (void)alloc;
    g_mock_tls_factory_calls++;
    memset(&g_mock_tls, 0, sizeof g_mock_tls);
    g_mock_tls.handshake = mock_tls_handshake;
    g_mock_tls.read      = mock_tls_read;
    g_mock_tls.write     = mock_tls_write;
    g_mock_tls.shutdown  = mock_tls_shutdown;
    g_mock_tls.pending   = mock_tls_pending;
    g_mock_tls.reset     = mock_tls_reset;
    g_mock_tls.destroy   = g_mock_destroy_null ? NULL : mock_tls_destroy;
    if (g_mock_set_hostname_mode == 0)
        g_mock_tls.set_hostname = mock_tls_set_hostname;
    else if (g_mock_set_hostname_mode == 2)
        g_mock_tls.set_hostname = mock_tls_set_hostname_fail;
    else
        g_mock_tls.set_hostname = NULL;   /* mode 1: absent */
    return &g_mock_tls;
}

/* A non-NULL dummy ctx pointer: the mock factory never dereferences it. */
static int g_mock_tls_ctx_dummy;
static KlTlsConfig g_mock_tls_cfg = {
    .ctx     = (KlTlsCtx *)&g_mock_tls_ctx_dummy,
    .factory = mock_tls_factory,
};

/* Reset every mock-TLS knob to its default. Called at the top of each test that
 * touches the mock (utest has no shared fixture teardown here). */
static void mock_tls_reset_globals(void)
{
    g_mock_tls_factory_calls = 0;
    g_mock_destroy_null      = 0;
    g_mock_set_hostname_mode = 0;
    g_mock_handshake_result  = KL_TLS_OK;
}

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
 * These exercise the REAL smtp_do_auth_plain success AND failure exits via the
 * gated test seam (-DHL_SMTP_TEST_HOOKS): smtp_test_auth_send REPLACES the live
 * smtp_command() so no server is needed, and captures the built command to
 * prove the live secret was present; smtp_test_auth_probe fires at the very end
 * of cleanup (after the three smtp_secure_zero calls) to prove BOTH exits reach
 * cleanup and every secret buffer is zeroed.
 * ════════════════════════════════════════════════════════════════════ */

/* Captured by the send hook; asserted by the test. */
static char   g_auth_cmd_capture[HL_SMTP_SEND_BUF_SIZE];
static size_t g_auth_cmd_capture_len;
static int    g_auth_send_calls;
static int    g_auth_send_return;      /* code the send hook returns */

/* Probe state: set by the probe hook, asserted by the test. */
static int    g_auth_probe_fired;
static int    g_auth_probe_all_zero;   /* 1 iff all three buffers were fully 0 */

static int auth_send_hook(HlSmtpTransport *t, const char *cmd,
                          int expected, int timeout)
{
    (void)t; (void)expected; (void)timeout;   /* transport is a passthrough dummy */
    g_auth_send_calls++;
    size_t n = strlen(cmd);
    if (n >= sizeof g_auth_cmd_capture) n = sizeof g_auth_cmd_capture - 1;
    memcpy(g_auth_cmd_capture, cmd, n);
    g_auth_cmd_capture[n] = '\0';
    g_auth_cmd_capture_len = n;
    return g_auth_send_return;
}

static void auth_probe_hook(const unsigned char *plain, size_t pn,
                            const char *b64, size_t bn,
                            const char *cmd, size_t cn)
{
    g_auth_probe_fired = 1;
    int all_zero = 1;
    for (size_t i = 0; i < pn; i++) if (plain[i] != 0) { all_zero = 0; break; }
    if (all_zero) for (size_t i = 0; i < bn; i++) if ((unsigned char)b64[i] != 0) { all_zero = 0; break; }
    if (all_zero) for (size_t i = 0; i < cn; i++) if ((unsigned char)cmd[i] != 0) { all_zero = 0; break; }
    g_auth_probe_all_zero = all_zero;
}

/* Compute the base64 the helper builds for (\0user\0pass), so the test can
 * assert the constructed command carried the live secret. */
static void expected_auth_b64(const char *user, const char *pass,
                              char *out, size_t out_size)
{
    unsigned char plain[1026];
    size_t ulen = strlen(user), plen = strlen(pass);
    plain[0] = '\0';
    memcpy(plain + 1, user, ulen);
    plain[1 + ulen] = '\0';
    memcpy(plain + 2 + ulen, pass, plen);
    size_t plain_len = 1 + ulen + 1 + plen;
    hl_smtp_base64_encode(plain, (int)plain_len, out, (int)out_size);
}

/* Success path: send hook returns 235; smtp_do_auth_plain returns 0, and the
 * probe sees all three secret buffers zeroed after cleanup. The captured
 * command contains "AUTH PLAIN <b64>" with the correct base64 of \0user\0pass
 * (proving construction + that the live secret was present at send time). */
UTEST(smtp_scrub, auth_plain_success_scrubs)
{
    g_auth_cmd_capture[0] = '\0';
    g_auth_cmd_capture_len = 0;
    g_auth_send_calls = 0;
    g_auth_send_return = 235;
    g_auth_probe_fired = 0;
    g_auth_probe_all_zero = 0;

    smtp_test_auth_send  = auth_send_hook;
    smtp_test_auth_probe = auth_probe_hook;

    const char *user = "alice@example.com";
    const char *pass = "s3cr3t-p@ssw0rd";
    char expected_b64[1400];
    expected_auth_b64(user, pass, expected_b64, sizeof expected_b64);
    char expected_cmd[HL_SMTP_SEND_BUF_SIZE];
    snprintf(expected_cmd, sizeof expected_cmd, "AUTH PLAIN %s", expected_b64);

    const char *err = NULL;
    int rc = smtp_do_auth_plain(NULL, user, pass, 5000, &err);
    ASSERT_EQ(rc, 0);

    /* The command the helper built + handed to the send hook carried the live
     * secret (correct base64 of \0user\0pass). */
    ASSERT_EQ(g_auth_send_calls, 1);
    ASSERT_TRUE(strstr(g_auth_cmd_capture, expected_cmd) != NULL);

    /* Cleanup ran and scrubbed every secret buffer. */
    ASSERT_TRUE(g_auth_probe_fired);
    ASSERT_TRUE(g_auth_probe_all_zero);

    smtp_test_auth_send  = NULL;
    smtp_test_auth_probe = NULL;
}

/* Failure path: send hook returns -1; smtp_do_auth_plain returns -1 with
 * err == "auth_failed", and the probe still sees all three buffers zeroed
 * (the failure exit reaches the same cleanup). */
UTEST(smtp_scrub, auth_plain_failure_scrubs)
{
    g_auth_cmd_capture[0] = '\0';
    g_auth_cmd_capture_len = 0;
    g_auth_send_calls = 0;
    g_auth_send_return = -1;
    g_auth_probe_fired = 0;
    g_auth_probe_all_zero = 0;

    smtp_test_auth_send  = auth_send_hook;
    smtp_test_auth_probe = auth_probe_hook;

    const char *user = "bob@example.com";
    const char *pass = "hunter2-secret";
    char expected_b64[1400];
    expected_auth_b64(user, pass, expected_b64, sizeof expected_b64);

    const char *err = NULL;
    int rc = smtp_do_auth_plain(NULL, user, pass, 5000, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(err != NULL);
    ASSERT_STREQ(err, "auth_failed");

    /* The secret was present in the built command before the failing send. */
    ASSERT_EQ(g_auth_send_calls, 1);
    ASSERT_TRUE(strstr(g_auth_cmd_capture, expected_b64) != NULL);

    /* The failure exit still reaches cleanup and scrubs. */
    ASSERT_TRUE(g_auth_probe_fired);
    ASSERT_TRUE(g_auth_probe_all_zero);

    smtp_test_auth_send  = NULL;
    smtp_test_auth_probe = NULL;
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

/* STARTTLS peer: greet 220, expect EHLO (reply 250), expect STARTTLS and reply
 * "220 go\r\n". When m->extra_after_220 is set, the 220 is IMMEDIATELY FOLLOWED
 * by junk plaintext bytes in the SAME write, so the client's plaintext
 * accumulator holds bytes past the 220 when starttls() is called (the
 * buffer-abort test). When clear, only the bare "220 go\r\n" is sent, so the
 * accumulator is EMPTY after the client consumes the 220 (the clean-STARTTLS
 * path the vtable/set_hostname rejection tests need). */
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
    if (m->extra_after_220)
        /* 220 + junk in one write: the junk lands in the plaintext buffer. */
        mp_send(c, "220 go\r\nEVIL-INJECTED-PLAINTEXT\r\n");
    else
        /* bare 220: the accumulator is empty after the client reads it. */
        mp_send(c, "220 go\r\n");
    /* Keep the socket open a moment so the client reads the coalesced bytes. */
    usleep(100 * 1000);
    close(c);
    return NULL;
}

/* Zero the peer, bind a loopback listener, and record its port - WITHOUT
 * spawning the accept thread yet, so a caller can set behavior knobs (e.g.
 * extra_after_220) before mp_spawn starts the thread that reads them. */
static int mp_setup(MockPeer *m)
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
    return 0;
}

static int mp_spawn(MockPeer *m, void *(*fn)(void *))
{
    return pthread_create(&m->tid, NULL, fn, m) == 0 ? 0 : -1;
}

/* Setup + spawn in one call (for peers that need no pre-spawn configuration). */
static int mp_start(MockPeer *m, void *(*fn)(void *))
{
    if (mp_setup(m) != 0) return -1;
    return mp_spawn(m, fn);
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

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", m.port, 10000, NULL, NULL, NULL, NULL);
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

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", m.port, 10000, NULL, NULL, NULL, NULL);
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

/* A THROTTLED-DRAIN peer: greet 220, then read forever but at a fixed, slow
 * byte rate (sleep MP_THROTTLE_SLEEP_US after every MP_THROTTLE_QUANTUM bytes,
 * independent of per-read size, so the rate is deterministic regardless of
 * socket buffering). It reads until EOF, so the CLIENT decides when the peer
 * goes away (by closing its fd), not a timer.
 *
 * This is what makes the deadline regression BITE. A fully-stalled ("never
 * drain") peer does NOT distinguish the fix from the bug: the pre-fix code
 * (a fresh timeout_ms budget minted per pump_until) still returns -1 after the
 * FIRST budget expires, exactly like the fixed single-absolute-deadline code.
 * A peer that drains slowly-but-steadily separates them: each ~256 KiB chunk
 * drains inside one fresh budget, so the PRE-FIX code keeps resetting the budget
 * and DELIVERS the whole payload (returns 0) after N x per-chunk time; the FIXED
 * code trips its one absolute deadline mid-write and returns -1 at ~one budget. */
#define MP_THROTTLE_QUANTUM   (64 * 1024)   /* bytes per sleep slice */
#define MP_THROTTLE_SLEEP_US  (30 * 1000)   /* 30 ms => ~2.1 MB/s ceiling */
static void *mp_slow_drain_thread(void *arg)
{
    MockPeer *m = arg;
    int c = accept(m->listen_fd, NULL, NULL);
    if (c < 0) return NULL;
    m->accepted = 1;
    if (m->greet)
        mp_send(c, "220 throttle ESMTP\r\n");

    char buf[65536];
    size_t since_sleep = 0;
    for (;;) {
        ssize_t n = read(c, buf, sizeof buf);
        if (n <= 0) break;                  /* client closed => EOF => done */
        m->total_read += (size_t)n;
        since_sleep += (size_t)n;
        /* usleep never sleeps LESS than requested, so the rate is bounded above
         * by QUANTUM / SLEEP regardless of read granularity or loopback speed. */
        while (since_sleep >= MP_THROTTLE_QUANTUM) {
            usleep(MP_THROTTLE_SLEEP_US);
            since_sleep -= MP_THROTTLE_QUANTUM;
        }
    }
    close(c);
    return NULL;
}

/* Process-level watchdog: a genuine future hang in the write path must FAIL CI
 * (process terminates non-zero) rather than hang forever. The pre-fix code does
 * not hang (it delivers the payload over many budgets), so the rc + elapsed
 * assertions below are the real discriminator; this SIGALRM is a finite backstop
 * against a different, hanging regression. Set well above any legitimate run. */
static void deadline_watchdog_fired(int sig)
{
    (void)sig;
    static const char msg[] =
        "FATAL: write_deadline_is_one_absolute_bound watchdog fired - "
        "hl_smtp_transport_write did not return within the watchdog window\n";
    ssize_t w = write(STDERR_FILENO, msg, sizeof msg - 1);
    (void)w;
    _exit(99);
}

/* Blocker 3 regression (genuinely biting): a 10 MiB write (~40 x 256 KiB chunks)
 * through a peer throttled to ~2 MB/s cannot complete inside a single 500 ms
 * budget, so it MUST fail with rc == -1 at ~one budget. The pre-fix per-pump
 * budget would instead reset the deadline on each ~120 ms chunk drain and
 * SUCCEED (rc == 0) after ~5 s. Both the return code (-1 vs 0) and the elapsed
 * time (~0.5 s vs ~5 s) separate the fixed code from the reverted code; the
 * SIGALRM backstop bounds any hanging future regression. */
UTEST(smtp_write, write_deadline_is_one_absolute_bound)
{
    MockPeer m;
    ASSERT_EQ(mp_start(&m, mp_slow_drain_thread), 0);

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", m.port, 10000, NULL, NULL, NULL, NULL);
    ASSERT_TRUE(t != NULL);
    char resp[HL_SMTP_RECV_BUF_SIZE];
    ASSERT_EQ(hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000), 220);

    size_t payload = (size_t)HL_SMTP_MAX_MSG_SIZE;   /* 10 MiB: ~40 x 256 KiB chunks */
    char *buf = malloc(payload);
    ASSERT_TRUE(buf != NULL);
    memset(buf, 'S', payload);

    const int write_timeout = 500;      /* ms: the ONE absolute budget */
    const unsigned watchdog_sec = 30;   /* finite backstop for a hanging regression */

    /* Arm the watchdog around the single write() only. */
    void (*prev)(int) = signal(SIGALRM, deadline_watchdog_fired);
    alarm(watchdog_sec);

    uint64_t start = kl_monotonic_ms();
    int rc = hl_smtp_transport_write(t, buf, payload, write_timeout);
    uint64_t elapsed = kl_monotonic_ms() - start;

    alarm(0);                           /* disarm: the write returned */
    signal(SIGALRM, prev);

    /* Fixed: fails at ONE absolute deadline. Reverted: would deliver all 10 MiB
     * (rc == 0) over ~5 s of reset budgets. Assert BOTH facets. */
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(elapsed < 2000);        /* ~one 500 ms budget, not ~5 s of them */

    /* Failure path: go straight to abortive free() (the production path a failed
     * write takes); do NOT hl_smtp_transport_shutdown() - a graceful drain of an
     * undrainable queue is not what a failed write does. free() closing the fd
     * gives the peer EOF, so its read loop ends and the thread joins. */
    ASSERT_EQ(hl_smtp_transport_free(t), 0);
    free(buf);
    mp_join(&m);
}

/* Drive a mock STARTTLS peer to the point just after the client consumed the
 * STARTTLS 220 reply, yielding a connected transport (via *out) ready for
 * starttls(). Set extra_junk to have the peer coalesce plaintext junk after the
 * 220 (so the accumulator is non-empty), or clear for a bare 220 (empty
 * accumulator). Void so the utest ASSERT_* macros (which `return;` on failure)
 * work here, mirroring chunked_delivery_case. On any failure *out is NULL. */
static void drive_to_starttls(int *utest_result, MockPeer *m, int extra_junk,
                              HlSmtpTransport **out)
{
    *out = NULL;
    ASSERT_EQ_MSG(mp_setup(m), 0, "mp_setup");
    m->extra_after_220 = extra_junk;
    ASSERT_EQ_MSG(mp_spawn(m, mp_starttls_junk_thread), 0, "mp_spawn");

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", m->port, 10000, NULL, NULL, NULL, NULL);
    ASSERT_TRUE_MSG(t != NULL, "connect");

    char resp[HL_SMTP_RECV_BUF_SIZE];
    ASSERT_EQ_MSG(hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000),
                  220, "greeting");
    ASSERT_EQ_MSG(hl_smtp_transport_write(t, "EHLO localhost\r\n", 16, 10000),
                  0, "EHLO write");
    ASSERT_EQ_MSG(hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000),
                  250, "EHLO reply");
    ASSERT_EQ_MSG(hl_smtp_transport_write(t, "STARTTLS\r\n", 10, 10000),
                  0, "STARTTLS write");
    ASSERT_EQ_MSG(hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000),
                  220, "STARTTLS reply");
    *out = t;
}

/* STARTTLS with unexpected buffered plaintext past the 220 must abort
 * fail-closed (fix 6): hl_smtp_transport_starttls returns -1 and does NOT
 * upgrade. Round-2: prove it is the BUFFER boundary (not a NULL cfg) that
 * aborts. The peer coalesces junk after the 220, so the accumulator is
 * non-empty when starttls() is called with a VALID mock cfg; the abort must
 * fire BEFORE the factory, so g_mock_tls_factory_calls stays 0. */
UTEST(smtp_starttls, unexpected_buffered_bytes_aborts)
{
    mock_tls_reset_globals();

    MockPeer m;
    HlSmtpTransport *t;
    drive_to_starttls(utest_result, &m, /*extra_junk=*/1, &t);
    if (!t) return;

    /* The coalesced junk landed in the plaintext accumulator. */
    ASSERT_NE(t->acc.len, (size_t)0);

    /* Upgrade with a VALID mock cfg: the buffered-bytes check must abort BEFORE
     * ever reaching the factory / handshake, distinguishing the buffer-boundary
     * abort from a NULL-cfg abort. */
    g_mock_tls_factory_calls = 0;
    int rc = hl_smtp_transport_starttls(t, "127.0.0.1", &g_mock_tls_cfg, 5000);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(g_mock_tls_factory_calls, 0);          /* aborted before TLS work */
    ASSERT_EQ(hl_smtp_transport_tls_active(t), 0);

    hl_smtp_transport_free(t);
    mp_join(&m);
}

/* A malformed KlTls vtable (destroy is NULL) returned by the factory must be
 * rejected without crashing (blocker 1): the reject path NULL-checks destroy.
 * Clean peer (bare 220, empty accumulator) so the buffer check passes and the
 * factory IS reached. */
UTEST(smtp_starttls, malformed_vtable_rejected_no_crash)
{
    mock_tls_reset_globals();
    g_mock_destroy_null = 1;   /* factory returns a KlTls whose destroy is NULL */

    MockPeer m;
    HlSmtpTransport *t;
    drive_to_starttls(utest_result, &m, /*extra_junk=*/0, &t);
    if (!t) return;

    ASSERT_EQ(t->acc.len, (size_t)0);   /* empty: buffer check passes */

    int rc = hl_smtp_transport_starttls(t, "127.0.0.1", &g_mock_tls_cfg, 5000);
    ASSERT_EQ(rc, -1);                          /* invalid vtable rejected */
    ASSERT_EQ(g_mock_tls_factory_calls, 1);     /* factory WAS reached */
    ASSERT_EQ(hl_smtp_transport_tls_active(t), 0);

    hl_smtp_transport_free(t);
    mp_join(&m);
}

/* A non-empty host REQUIRES set_hostname to exist: an absent hook must reject
 * (blocker 2), else the handshake would run without the expected identity and
 * defeat verify-full. Clean peer + empty accumulator. */
UTEST(smtp_starttls, missing_set_hostname_rejected)
{
    mock_tls_reset_globals();
    g_mock_set_hostname_mode = 1;   /* set_hostname absent (NULL hook) */

    MockPeer m;
    HlSmtpTransport *t;
    drive_to_starttls(utest_result, &m, /*extra_junk=*/0, &t);
    if (!t) return;

    ASSERT_EQ(t->acc.len, (size_t)0);

    int rc = hl_smtp_transport_starttls(t, "127.0.0.1", &g_mock_tls_cfg, 5000);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(g_mock_tls_factory_calls, 1);     /* reached the factory */
    ASSERT_EQ(hl_smtp_transport_tls_active(t), 0);

    hl_smtp_transport_free(t);
    mp_join(&m);
}

/* A non-empty host also REQUIRES set_hostname to SUCCEED: a hook that returns
 * -1 must reject (blocker 2). Clean peer + empty accumulator. */
UTEST(smtp_starttls, failing_set_hostname_rejected)
{
    mock_tls_reset_globals();
    g_mock_set_hostname_mode = 2;   /* set_hostname present but returns -1 */

    MockPeer m;
    HlSmtpTransport *t;
    drive_to_starttls(utest_result, &m, /*extra_junk=*/0, &t);
    if (!t) return;

    ASSERT_EQ(t->acc.len, (size_t)0);

    int rc = hl_smtp_transport_starttls(t, "127.0.0.1", &g_mock_tls_cfg, 5000);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(g_mock_tls_factory_calls, 1);     /* reached the factory */
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
    HlSmtpTransport *t = hl_smtp_transport_connect("192.0.2.1", 25, 600, NULL, NULL, NULL, NULL);
    /* Bounded failure: NULL (connect / deadline). hl_smtp_transport_connect
     * cancels + waits for detachment internally before freeing, so a clean
     * return here already proves detachment did not hang. */
    ASSERT_TRUE(t == NULL);
}

/* ════════════════════════════════════════════════════════════════════
 *  Post-resolution operation deadline (Dop, section 8) - deterministic
 *  deadline-versus-cancel precedence.
 *
 *  The frozen precedence (pump_check): a completed predicate, THEN an expired
 *  Dop, THEN cancellation, THEN the stage budget. So when Dop and cancellation
 *  are BOTH ready at one pump checkpoint, Dop wins and the terminal is tagged
 *  post_resolution_deadline (never cancelled). These tests exercise that exact
 *  same-checkpoint race deterministically, on ONE clock domain (the real
 *  kl_monotonic_ms()): no virtual clock is advanced independently of Keel's
 *  timers - the checkpoint is simply driven to a state where both conditions
 *  hold at the identical evaluation.
 *
 *  REVERT PROOF for both: reorder pump_check so the cancel branch precedes the
 *  Dop branch and both assertions on dop_expired==1 flip to failure (cancel
 *  would win the race and the terminal would be mis-tagged terminal:cancelled).
 * ════════════════════════════════════════════════════════════════════ */

static int dop_never_done(HlSmtpTransport *t) { (void)t; return 0; }
static int dop_always_cancel(void *user) { (void)user; return 1; }

/* Deterministic core: call the frozen precedence classifier directly with Dop
 * and cancellation BOTH ready at one checkpoint. No event loop, no sockets, no
 * wall-clock dependence - the definitive same-checkpoint proof. */
UTEST(smtp_dop, precedence_dop_beats_cancel_at_one_checkpoint)
{
    HlSmtpTransport t;
    memset(&t, 0, sizeof t);
    t.dop_ms      = kl_monotonic_ms();   /* already reached: pump_check re-reads now >= dop_ms */
    t.cancel_poll = dop_always_cancel;   /* cancellation ALSO ready at this checkpoint */
    t.cancel_user = NULL;

    /* Stage budget far in the future so only Dop-vs-cancel can terminate. */
    int c = pump_check(&t, dop_never_done, UINT64_MAX);

    ASSERT_EQ(c, -1);                    /* terminate */
    ASSERT_EQ(t.dop_expired, 1);         /* Dop classified - NOT cancellation */
}

/* Live-pump variant: the same race, but reached through the real pump loop via
 * the checkpoint hook (which also validates the hook that the resolver/provider
 * tests build on). A peer accepts then withholds the greeting, so read_reply
 * pumps; the hook, once armed, expires Dop AND arms cancellation at the TOP of a
 * single pump_check so both are ready at that one checkpoint. */
static volatile int g_dop_live_arm;      /* test arms after connect completes */
static volatile int g_dop_live_armed;    /* latch: the hook fires the race once */
static volatile int g_dop_live_cancel;   /* what the transport's cancel_poll returns */
static int          g_dop_live_armed_at; /* checkpoint index the hook armed at (observe) */

static int dop_live_cancel_poll(void *user) { (void)user; return g_dop_live_cancel; }

static void dop_live_checkpoint(HlSmtpTransport *t, unsigned idx)
{
    if (g_dop_live_arm && !g_dop_live_armed) {
        /* Align both readiness conditions onto THIS checkpoint, on the real
         * clock: overwrite Dop to now (expired - pump_check re-reads now >= dop_ms
         * microseconds later) and make the pending cancel ready. */
        g_dop_live_armed   = 1;
        t->dop_ms          = kl_monotonic_ms();
        g_dop_live_cancel  = 1;
        g_dop_live_armed_at = (int)idx;
    }
}

/* A peer that accepts and holds the connection open WITHOUT greeting, so the
 * client's read_reply keeps pumping (giving the hook checkpoints to fire on). */
static void *mp_accept_and_hold_thread(void *arg)
{
    MockPeer *m = arg;
    int c = accept(m->listen_fd, NULL, NULL);
    if (c < 0) return NULL;
    m->accepted = 1;
    /* Withhold the greeting; drain anything the client sends until it closes. */
    char buf[4096];
    for (;;) {
        ssize_t n = read(c, buf, sizeof buf);
        if (n <= 0) break;
    }
    close(c);
    return NULL;
}

static void dop_live_watchdog_fired(int sig)
{
    (void)sig;
    static const char msg[] =
        "FATAL: precedence_dop_beats_cancel_live_pump watchdog fired - "
        "read_reply did not return within the watchdog window\n";
    ssize_t w = write(STDERR_FILENO, msg, sizeof msg - 1);
    (void)w;
    _exit(70);
}

UTEST(smtp_dop, precedence_dop_beats_cancel_live_pump)
{
    MockPeer m;
    ASSERT_EQ(mp_start(&m, mp_accept_and_hold_thread), 0);

    /* Generous connect/read timeout: the DEADLINE under test is Dop, armed by the
     * hook, not this stage budget. */
    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", m.port, 10000,
                                                   dop_live_cancel_poll, NULL,
                                                   NULL, NULL);
    ASSERT_TRUE(t != NULL);

    /* Arm the same-checkpoint race only now that connect is done. */
    g_dop_live_armed    = 0;
    g_dop_live_cancel   = 0;
    g_dop_live_armed_at = -1;
    smtp_test_checkpoint = dop_live_checkpoint;
    g_dop_live_arm      = 1;

    const unsigned watchdog_sec = 30;    /* finite backstop for a hanging regression */
    void (*prev)(int) = signal(SIGALRM, dop_live_watchdog_fired);
    alarm(watchdog_sec);

    char resp[HL_SMTP_RECV_BUF_SIZE];
    int code = hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 10000);

    alarm(0);
    signal(SIGALRM, prev);

    /* Disarm the hook BEFORE any further pumps (shutdown/free pump too). */
    smtp_test_checkpoint = NULL;
    g_dop_live_arm       = 0;

    ASSERT_EQ(code, -1);                 /* read terminated */
    ASSERT_EQ(hl_smtp_transport_dop_expired(t), 1);  /* by Dop, not cancellation */
    ASSERT_TRUE(g_dop_live_armed_at >= 0);           /* the hook did fire the race */

    hl_smtp_transport_free(t);
    mp_join(&m);
}

UTEST_MAIN()
