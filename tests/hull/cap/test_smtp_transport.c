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
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <keel/tls.h>   /* KlTls / KlTlsConfig: mock the TLS session seam */
#include <keel/allocator.h>       /* kl_allocator_default (borrowed by the TLS ctx) */
#include <keel_tls_mbedtls.h>     /* real in-process mbedTLS peer + client ctx */
#include "smtp_tls_test_certs.h"  /* fixed test-only certs/keys (TEST-ONLY) */

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

/* ════════════════════════════════════════════════════════════════════
 *  Resolver hook - cancellation during resolution takes the post-DNS cancel
 *  path BEFORE any socket attempt.
 *
 *  The injected resolver fills the address list in exact order and, at its
 *  release point, arms cancellation (modeling a cancel that arrived during the
 *  blocking resolve). hl_smtp_transport_connect must then abort before the
 *  connect op is initialized - so the pump never runs and no socket() is called.
 *  We observe "no pump ran" via a checkpoint counter that stays 0 (pump_check,
 *  where socket attempts are driven, is never reached).
 *
 *  REVERT PROOF: delete the post-DNS cancel check in hl_smtp_transport_connect
 *  and the connect op starts + pumps - the checkpoint counter goes non-zero and
 *  this assertion flips to failure.
 * ════════════════════════════════════════════════════════════════════ */

static volatile int g_res_cancel;        /* what the transport's cancel_poll returns */
static int          g_res_checkpoints;   /* pump_check invocations during the connect */

static int res_cancel_poll(void *user) { (void)user; return g_res_cancel; }
static void res_count_checkpoint(HlSmtpTransport *t, unsigned idx)
{ (void)t; (void)idx; g_res_checkpoints++; }

/* Inject one loopback address in exact order, then arm cancellation as the
 * blocking resolve "returns" (its release point). */
static int res_inject_then_cancel(HlSmtpTransport *t, const char *host, int port)
{
    (void)host;
    uint8_t ip[4] = { 127, 0, 0, 1 };
    if (kl_sockaddr_from_ipv4(&t->addrs[0], ip, (uint16_t)port) != 0)
        return 0;
    g_res_cancel = 1;   /* cancel observed during resolution */
    return 1;
}

UTEST(smtp_dop, cancellation_during_resolution_no_socket_attempt)
{
    g_res_cancel      = 0;
    g_res_checkpoints = 0;
    smtp_test_resolve    = res_inject_then_cancel;
    smtp_test_checkpoint = res_count_checkpoint;

    const unsigned watchdog_sec = 30;
    void (*prev)(int) = signal(SIGALRM, dop_live_watchdog_fired);
    alarm(watchdog_sec);

    /* Host is never really resolved (the hook replaces getaddrinfo); a bogus port
     * target would still never be dialed because cancel aborts first. */
    HlSmtpTransport *t = hl_smtp_transport_connect("mail.example.test", 25, 5000,
                                                   res_cancel_poll, NULL, NULL, NULL);

    alarm(0);
    signal(SIGALRM, prev);

    smtp_test_resolve    = NULL;
    smtp_test_checkpoint = NULL;

    ASSERT_TRUE(t == NULL);              /* cancelled -> NULL (connect_failed at the caller) */
    ASSERT_EQ(g_res_checkpoints, 0);     /* returned before any pump -> before any socket attempt */
}

/* ════════════════════════════════════════════════════════════════════
 *  Provider / attempt hook - deterministic pending-connect Dop and a real
 *  Happy-Eyeballs stagger, with no environmental skip.
 *
 *  The fake provider (smtp_test_socket_provider) hands the connect op real fds
 *  the event loop can watch, keeping ONE clock domain: a "pending" address is a
 *  socketpair whose send buffer is filled so its fd is never write-ready (both
 *  epoll and kqueue respect a full send buffer), so the attempt stays pending
 *  under kernel semantics - not a timing hack. "succeed" / "fail" leave the fd
 *  writable and let get_so_error report SO_ERROR. Attempts are recorded (address
 *  index + monotonic timestamp) so the stagger can be observed.
 * ════════════════════════════════════════════════════════════════════ */

enum { FK_PENDING = 0, FK_SUCCEED = 1, FK_FAIL = 2 };

typedef struct { int used; int a; int b; int disp; int idx; } FkSlot;
static FkSlot   g_fk[32];
static int      g_fk_disp[KL_CONNECT_MAX_ADDRS];  /* per-address disposition */
static int      g_fk_order[KL_CONNECT_MAX_ADDRS]; /* attempt order (address index) */
static uint64_t g_fk_ts[KL_CONNECT_MAX_ADDRS];    /* attempt timestamps (monotonic ms) */
static int      g_fk_n;                            /* number of connect() attempts */
static int      g_fk_succeeded_idx;               /* address index that read SO_ERROR==0, or -1 */

static void fk_reset(void)
{
    memset(g_fk, 0, sizeof g_fk);
    memset(g_fk_disp, 0, sizeof g_fk_disp);
    memset(g_fk_order, 0, sizeof g_fk_order);
    memset(g_fk_ts, 0, sizeof g_fk_ts);
    g_fk_n = 0;
    g_fk_succeeded_idx = -1;
}

static FkSlot *fk_slot_for(int a)
{
    for (size_t i = 0; i < sizeof g_fk / sizeof g_fk[0]; i++)
        if (g_fk[i].used && g_fk[i].a == a) return &g_fk[i];
    return NULL;
}

static KlSocketHandle fk_socket(void *c, int d, int ty, int p)
{
    (void)c; (void)d; (void)ty; (void)p;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return KL_INVALID_SOCKET;
    int small = 2048;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    for (size_t i = 0; i < sizeof g_fk / sizeof g_fk[0]; i++)
        if (!g_fk[i].used) {
            g_fk[i].used = 1; g_fk[i].a = sv[0]; g_fk[i].b = sv[1];
            g_fk[i].disp = -1; g_fk[i].idx = -1;
            return (KlSocketHandle)sv[0];
        }
    close(sv[0]); close(sv[1]);
    return KL_INVALID_SOCKET;
}
static int  fk_set_nb(void *c, KlSocketHandle fd)
{ (void)c; int a = (int)fd; int fl = fcntl(a, F_GETFL, 0); return fcntl(a, F_SETFL, fl | O_NONBLOCK); }
static int  fk_set_int(void *c, KlSocketHandle fd, int on) { (void)c; (void)fd; (void)on; return 0; }
static void fk_set_void(void *c, KlSocketHandle fd) { (void)c; (void)fd; }
static int  fk_get_local(void *c, KlSocketHandle fd, KlSockAddr *out)
{ (void)c; (void)fd; uint8_t ip[4] = {127,0,0,1}; return kl_sockaddr_from_ipv4(out, ip, 0); }

static int fk_connect(void *c, KlSocketHandle fd, const KlSockAddr *addr)
{
    (void)c;
    int a = (int)fd;
    int idx = (addr->addr_len == 4) ? (addr->u.ip[3] - 1) : 0;
    if (idx < 0 || idx >= KL_CONNECT_MAX_ADDRS) idx = 0;
    int disp = g_fk_disp[idx];
    if (g_fk_n < KL_CONNECT_MAX_ADDRS) {
        g_fk_order[g_fk_n] = idx;
        g_fk_ts[g_fk_n]    = kl_monotonic_ms();
        g_fk_n++;
    }
    FkSlot *s = fk_slot_for(a);
    if (s) { s->disp = disp; s->idx = idx; }
    if (disp == FK_PENDING) {
        /* Fill the send buffer so 'a' never becomes write-ready (the attempt stays
         * pending under kernel semantics until the Dop / stagger timer fires). */
        char buf[2048];
        memset(buf, 'x', sizeof buf);
        int fl = fcntl(a, F_GETFL, 0); fcntl(a, F_SETFL, fl | O_NONBLOCK);
        while (write(a, buf, sizeof buf) > 0) { }
    }
    /* Report async-in-progress; a writable 'a' (succeed/fail) then drives
     * co_connect_watcher -> get_so_error. */
    errno = EINPROGRESS;
    return -1;
}
static int fk_get_so_error(void *c, KlSocketHandle fd, int *out)
{
    (void)c;
    FkSlot *s = fk_slot_for((int)fd);
    if (s && s->disp == FK_FAIL) { *out = ECONNREFUSED; }
    else { *out = 0; if (s) g_fk_succeeded_idx = s->idx; }
    return 0;
}
static kl_ssize_t fk_send(void *c, KlSocketHandle fd, const void *b, size_t n)
{ (void)c; return send((int)fd, b, n, 0); }
static kl_ssize_t fk_recv(void *c, KlSocketHandle fd, void *b, size_t n)
{ (void)c; return recv((int)fd, b, n, 0); }
static kl_ssize_t fk_recv_peek(void *c, KlSocketHandle fd, void *b, size_t n)
{ (void)c; return recv((int)fd, b, n, MSG_PEEK); }
static int fk_close(void *c, KlSocketHandle fd)
{
    (void)c; int a = (int)fd;
    FkSlot *s = fk_slot_for(a);
    if (s) { close(s->b); s->used = 0; }
    return close(a);
}

static const KlSocketOps FK_OPS = {
    .set_nonblocking = fk_set_nb,
    .set_blocking    = NULL,
    .set_cloexec     = fk_set_void,
    .set_nosigpipe   = fk_set_void,
    .set_reuseaddr   = fk_set_int,
    .set_reuseport   = fk_set_int,
    .set_ipv6only    = fk_set_int,
    .set_tcp_nodelay = fk_set_int,
    .set_cork        = fk_set_int,
    .socket          = fk_socket,
    .connect         = fk_connect,
    .close           = fk_close,
    .get_local_addr  = fk_get_local,
    .get_so_error    = fk_get_so_error,
    .send            = fk_send,
    .recv            = fk_recv,
    .recv_peek       = fk_recv_peek,
};
static const KlSocketProvider FK_PROVIDER = { .ops = &FK_OPS, .context = NULL };

/* Inject N addresses 10.11.12.(i+1) in order; disposition comes from g_fk_disp. */
static int g_fk_naddr;
static int fk_resolve(HlSmtpTransport *t, const char *host, int port)
{
    (void)host;
    for (int i = 0; i < g_fk_naddr && i < KL_CONNECT_MAX_ADDRS; i++) {
        uint8_t ip[4] = { 10, 11, 12, (uint8_t)(i + 1) };
        if (kl_sockaddr_from_ipv4(&t->addrs[i], ip, (uint16_t)port) != 0) return i;
    }
    return g_fk_naddr;
}

/* Deterministic pending-connect Dop: one address, permanently pending (filled
 * send buffer). The connect-deadline timer (armed with Dop-now) fires and the
 * connect fails with deadline_expired set - no network, no environmental skip.
 * REVERT PROOF: neuter the Dop classification (as in step 1) and dop_expired is
 * lost; more directly, this can only pass because the pending fd never completes,
 * which the filled-send-buffer guarantees on both epoll and kqueue. */
UTEST(smtp_dop, pending_connect_dop_expires_deterministically)
{
    fk_reset();
    g_fk_naddr   = 1;
    g_fk_disp[0] = FK_PENDING;
    smtp_test_resolve         = fk_resolve;
    smtp_test_socket_provider = &FK_PROVIDER;

    const unsigned watchdog_sec = 30;
    void (*prev)(int) = signal(SIGALRM, dop_live_watchdog_fired);
    alarm(watchdog_sec);

    uint64_t t0 = kl_monotonic_ms();
    int dop = 0;
    /* 400 ms Dop: short, deterministic, well under the watchdog. */
    HlSmtpTransport *t = hl_smtp_transport_connect("mail.example.test", 25, 400,
                                                   NULL, NULL, NULL, &dop);
    uint64_t elapsed = kl_monotonic_ms() - t0;

    alarm(0);
    signal(SIGALRM, prev);
    smtp_test_resolve         = NULL;
    smtp_test_socket_provider = NULL;

    ASSERT_TRUE(t == NULL);              /* connect failed */
    ASSERT_EQ(dop, 1);                   /* by the post-resolution operation deadline */
    ASSERT_GE(g_fk_n, 1);                /* at least one attempt was made */
    ASSERT_TRUE(elapsed < 5000);         /* bounded by Dop, not the stage/OS default */
}

/* Real Happy-Eyeballs stagger: address 0 pending, address 1 succeeds. The RFC 8305
 * Connection Attempt Delay timer (~250 ms) must fire and start address 1 while
 * address 0 is still pending; address 1 then wins. Observed via attempt order +
 * the inter-attempt gap (a real timer, on the real clock).
 * REVERT PROOF: drop SMTP_CONNECT_ATTEMPT_DELAY_MS toward 0 and the gap assertion
 * fails; make address 1 FK_FAIL and the winner assertion fails. */
UTEST(smtp_dop, happy_eyeballs_stagger_starts_second_address)
{
    fk_reset();
    g_fk_naddr   = 2;
    g_fk_disp[0] = FK_PENDING;
    g_fk_disp[1] = FK_SUCCEED;
    smtp_test_resolve         = fk_resolve;
    smtp_test_socket_provider = &FK_PROVIDER;

    const unsigned watchdog_sec = 30;
    void (*prev)(int) = signal(SIGALRM, dop_live_watchdog_fired);
    alarm(watchdog_sec);

    /* Generous Dop so the ~250 ms stagger (not the deadline) drives address 2. */
    HlSmtpTransport *t = hl_smtp_transport_connect("mail.example.test", 25, 5000,
                                                   NULL, NULL, NULL, NULL);

    alarm(0);
    signal(SIGALRM, prev);
    smtp_test_resolve         = NULL;
    smtp_test_socket_provider = NULL;

    ASSERT_TRUE(t != NULL);              /* address 1 won the race */
    ASSERT_EQ(g_fk_n, 2);               /* both addresses were attempted */
    ASSERT_EQ(g_fk_order[0], 0);        /* address 0 first */
    ASSERT_EQ(g_fk_order[1], 1);        /* address 1 second */
    ASSERT_EQ(g_fk_succeeded_idx, 1);   /* address 1 is the one that connected */
    /* The stagger timer fired before address 1 started: the gap is around the
     * ~250 ms Connection Attempt Delay (allow scheduling slack). */
    uint64_t gap = g_fk_ts[1] - g_fk_ts[0];
    ASSERT_GE(gap, (uint64_t)(SMTP_CONNECT_ATTEMPT_DELAY_MS - 50));

    hl_smtp_transport_free(t);
}

/* ════════════════════════════════════════════════════════════════════
 *  In-process mbedTLS peer (real handshake, fixed test-only certs).
 *
 *  No external openssl process and no network: a background thread accepts a
 *  loopback connection and drives a REAL mbedTLS server handshake with the
 *  embedded server cert/key, while the SMTP transport drives the client
 *  handshake through its event-loop pump. The client trusts CA1 (success) or an
 *  unrelated CA2 (unknown-CA failure); the server leaf is CN=localhost, so a
 *  verify hostname other than "localhost" is a mismatch failure. All TLS material
 *  and every helper below is TEST-ONLY (smtp_tls_test_certs.h).
 * ════════════════════════════════════════════════════════════════════ */

/* The TLS ctx borrows its allocator for its whole lifetime, so keep it static. */
static KlAllocator g_tls_alloc;
static int         g_tls_alloc_init;
static KlAllocator *tls_alloc(void)
{
    if (!g_tls_alloc_init) { g_tls_alloc = kl_allocator_default(); g_tls_alloc_init = 1; }
    return &g_tls_alloc;
}

typedef struct {
    int       listen_fd;
    int       port;
    pthread_t tid;
    /* knobs (set before spawn) */
    int starttls;          /* 1 = plaintext SMTP prelude, then STARTTLS, then handshake */
    int reject_starttls;   /* STARTTLS: reply 454 and never handshake */
    int inject_after_220;  /* STARTTLS: append plaintext after the 220 go */
    int stall_handshake;   /* never answer the ClientHello (drives the client to Dop) */
    /* observed */
    int accepted;
    int reached_handshake; /* the peer began driving a TLS handshake */
    int handshake_ok;      /* the server handshake returned OK */
    int post_fail_bytes;   /* bytes received from the client AFTER a rejected/aborted TLS */
} TlsPeer;

static int tls_peer_setup(TlsPeer *p)
{
    memset(p, 0, sizeof *p);
    p->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (p->listen_fd < 0) return -1;
    int one = 1;
    setsockopt(p->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(p->listen_fd, (struct sockaddr *)&sa, sizeof sa) < 0) return -1;
    socklen_t sl = sizeof sa;
    if (getsockname(p->listen_fd, (struct sockaddr *)&sa, &sl) < 0) return -1;
    p->port = ntohs(sa.sin_port);
    return listen(p->listen_fd, 1);
}

static void tls_peer_send(int fd, const char *s)
{ size_t n = strlen(s); ssize_t w = write(fd, s, n); (void)w; }

static void tls_peer_read_line(int fd, char *buf, int cap)
{
    int i = 0;
    while (i < cap - 1) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) break;
        buf[i++] = ch;
        if (ch == '\n') break;
    }
    buf[i] = '\0';
}

/* Count bytes the client sends within a short window (used to prove it sent NO
 * ClientHello after a rejected / aborted TLS). */
static int tls_peer_drain_briefly(int fd)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int total = 0; char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        total += (int)n;
    }
    return total;
}

/* Drive a server-side mbedTLS handshake to completion (poll-based, bounded). fd
 * must be non-blocking. Mirrors shared/tls_client.c::tls_handshake_loop. */
static int tls_peer_handshake(KlTls *tls, int fd, int timeout_ms)
{
    for (;;) {
        KlTlsResult r = tls->handshake(tls, fd);
        if (r == KL_TLS_OK)    return 0;
        if (r == KL_TLS_ERROR) return -1;
        short ev = (r == KL_TLS_WANT_READ) ? POLLIN : POLLOUT;
        struct pollfd pfd = { .fd = fd, .events = ev, .revents = 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) return -1;
    }
}

static void *tls_peer_thread(void *arg)
{
    TlsPeer *p = arg;
    int c = accept(p->listen_fd, NULL, NULL);
    if (c < 0) return NULL;
    p->accepted = 1;

    if (p->starttls) {
        char line[512];
        tls_peer_send(c, "220 test ESMTP\r\n");
        tls_peer_read_line(c, line, sizeof line);            /* EHLO */
        tls_peer_send(c, "250-test\r\n250 STARTTLS\r\n");
        tls_peer_read_line(c, line, sizeof line);            /* STARTTLS */
        if (p->reject_starttls) {
            tls_peer_send(c, "454 TLS not available\r\n");
            p->post_fail_bytes = tls_peer_drain_briefly(c);  /* must stay 0 */
            close(c);
            return NULL;
        }
        if (p->inject_after_220) {
            /* 220 + injected plaintext in one write: the client must abort before
             * the TLS factory and send NOTHING further. */
            tls_peer_send(c, "220 go\r\nINJECTED-PLAINTEXT-AFTER-220\r\n");
            p->post_fail_bytes = tls_peer_drain_briefly(c);  /* must stay 0 */
            close(c);
            return NULL;
        }
        tls_peer_send(c, "220 go\r\n");
    }

    /* Hand the fd to the TLS handshake drive (non-blocking for the WANT_* loop). */
    int fl = fcntl(c, F_GETFL, 0); fcntl(c, F_SETFL, fl | O_NONBLOCK);

    if (p->stall_handshake) {
        /* Never answer the ClientHello: read + hold until the client closes on
         * Dop. The client's first handshake flight arriving proves it reached
         * the handshake. */
        p->reached_handshake = 1;
        char buf[1024];
        for (;;) { ssize_t n = read(c, buf, sizeof buf); if (n == 0) break; if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break; }
        close(c);
        return NULL;
    }

    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)HL_TLS_SRV_CRT, sizeof HL_TLS_SRV_CRT,
        (const unsigned char *)HL_TLS_SRV_KEY, sizeof HL_TLS_SRV_KEY,
        NULL, 0, KL_MTLS_NONE, tls_alloc());
    if (!sctx) { close(c); return NULL; }
    KlTls *stls = kl_tls_mbedtls_create(sctx, tls_alloc());
    if (!stls) { kl_tls_mbedtls_ctx_destroy(sctx); close(c); return NULL; }

    p->reached_handshake = 1;
    p->handshake_ok = (tls_peer_handshake(stls, c, 10000) == 0);
    /* Absorb whatever the client sends next (close_notify / SMTP) until it closes,
     * so we can also count post-failure bytes on the caller side if needed. */
    (void)tls_peer_drain_briefly(c);

    stls->destroy(stls);
    kl_tls_mbedtls_ctx_destroy(sctx);
    close(c);
    return NULL;
}

static void tls_peer_join(TlsPeer *p)
{
    pthread_join(p->tid, NULL);
    close(p->listen_fd);
}

/* Build a client KlTlsConfig trusting @p ca_pem. The returned ctx is owned by the
 * caller and freed with kl_tls_mbedtls_ctx_destroy. */
static KlTlsConfig tls_client_cfg(KlTlsCtx **out_ctx, const char *ca_pem, size_t ca_len)
{
    KlTlsCtx *ctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca_pem, ca_len, tls_alloc());
    *out_ctx = ctx;
    KlTlsConfig cfg = { .ctx = ctx, .factory = kl_tls_mbedtls_create,
                        .ctx_destroy = kl_tls_mbedtls_ctx_destroy };
    return cfg;
}

/* Manually run the plaintext STARTTLS prelude on the transport, leaving it ready
 * for hl_smtp_transport_starttls (greeting + EHLO/250 + STARTTLS/220 consumed). */
static int tls_do_starttls_prelude(HlSmtpTransport *t)
{
    char resp[HL_SMTP_RECV_BUF_SIZE];
    if (hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 5000) != 220) return -1;
    if (hl_smtp_transport_write(t, "EHLO test\r\n", 11, 5000) != 0) return -1;
    if (hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 5000) != 250) return -1;
    if (hl_smtp_transport_write(t, "STARTTLS\r\n", 10, 5000) != 0) return -1;
    if (hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 5000) != 220) return -1;
    return 0;
}

static void tls_watchdog_fired(int sig)
{
    (void)sig;
    static const char msg[] = "FATAL: TLS peer test watchdog fired\n";
    ssize_t w = write(STDERR_FILENO, msg, sizeof msg - 1); (void)w;
    _exit(70);
}

/* Implicit TLS (SMTPS): a real mbedTLS handshake completes against the peer and
 * the transport reports an active TLS session.
 * REVERT PROOF: give the client CA2 instead of CA1 (unknown-CA test) or a verify
 * host other than "localhost" (hostname test) and this fails closed. */
UTEST(smtp_tls, implicit_tls_success)
{
    TlsPeer p; ASSERT_EQ(tls_peer_setup(&p), 0);
    ASSERT_EQ(pthread_create(&p.tid, NULL, tls_peer_thread, &p), 0);

    void (*prev)(int) = signal(SIGALRM, tls_watchdog_fired); alarm(30);

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", p.port, 10000,
                                                   NULL, NULL, NULL, NULL);
    ASSERT_TRUE(t != NULL);
    KlTlsCtx *cctx = NULL;
    KlTlsConfig cfg = tls_client_cfg(&cctx, HL_TLS_CA1_PEM, sizeof HL_TLS_CA1_PEM);
    int rc = hl_smtp_transport_implicit_tls(t, "localhost", &cfg, 10000);

    alarm(0); signal(SIGALRM, prev);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(hl_smtp_transport_tls_active(t), 1);

    hl_smtp_transport_shutdown(t);
    ASSERT_EQ(hl_smtp_transport_free(t), 0);   /* teardown confirmed exactly once */
    kl_tls_mbedtls_ctx_destroy(cctx);
    tls_peer_join(&p);
    ASSERT_EQ(p.handshake_ok, 1);
}

/* STARTTLS: after the plaintext prelude, the transport hands the socket to TLS
 * BEFORE the ClientHello (plaintext parsing never consumes handshake bytes), and
 * a real handshake completes. That the handshake succeeds is itself the proof the
 * socket ownership transferred cleanly at the ciphertext boundary. */
UTEST(smtp_tls, starttls_success)
{
    TlsPeer p; ASSERT_EQ(tls_peer_setup(&p), 0);
    p.starttls = 1;
    ASSERT_EQ(pthread_create(&p.tid, NULL, tls_peer_thread, &p), 0);

    void (*prev)(int) = signal(SIGALRM, tls_watchdog_fired); alarm(30);

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", p.port, 10000,
                                                   NULL, NULL, NULL, NULL);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(tls_do_starttls_prelude(t), 0);

    KlTlsCtx *cctx = NULL;
    KlTlsConfig cfg = tls_client_cfg(&cctx, HL_TLS_CA1_PEM, sizeof HL_TLS_CA1_PEM);
    int rc = hl_smtp_transport_starttls(t, "localhost", &cfg, 10000);

    alarm(0); signal(SIGALRM, prev);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(hl_smtp_transport_tls_active(t), 1);

    hl_smtp_transport_shutdown(t);
    ASSERT_EQ(hl_smtp_transport_free(t), 0);
    kl_tls_mbedtls_ctx_destroy(cctx);
    tls_peer_join(&p);
    ASSERT_EQ(p.handshake_ok, 1);
}

/* After a failed handshake the transport must be fail-closed: TLS not active, and
 * the stream is cancelled so NO plaintext read/write can proceed (no fallback),
 * and free confirms teardown exactly once. Shared by the failure tests. */
static void tls_assert_failed_closed(int *utest_result, HlSmtpTransport *t)
{
    ASSERT_EQ(hl_smtp_transport_tls_active(t), 0);
    char resp[HL_SMTP_RECV_BUF_SIZE];
    ASSERT_EQ(hl_smtp_transport_read_reply(t, resp, (int)sizeof resp, 500), -1);
    ASSERT_EQ(hl_smtp_transport_write(t, "MAIL FROM:<a@b>\r\n", 16, 500), -1);
    ASSERT_EQ(hl_smtp_transport_free(t), 0);   /* teardown confirmed exactly once */
}

/* Hostname mismatch fails closed: the server leaf is CN=localhost, so verifying a
 * different hostname rejects the handshake. No plaintext fallback.
 * REVERT PROOF: verify "localhost" (the implicit_tls_success host) and it passes. */
UTEST(smtp_tls, hostname_mismatch_fails_closed)
{
    TlsPeer p; ASSERT_EQ(tls_peer_setup(&p), 0);
    ASSERT_EQ(pthread_create(&p.tid, NULL, tls_peer_thread, &p), 0);

    void (*prev)(int) = signal(SIGALRM, tls_watchdog_fired); alarm(30);

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", p.port, 10000,
                                                   NULL, NULL, NULL, NULL);
    ASSERT_TRUE(t != NULL);
    KlTlsCtx *cctx = NULL;
    KlTlsConfig cfg = tls_client_cfg(&cctx, HL_TLS_CA1_PEM, sizeof HL_TLS_CA1_PEM);
    int rc = hl_smtp_transport_implicit_tls(t, "smtp.wrong.example", &cfg, 10000);

    alarm(0); signal(SIGALRM, prev);

    ASSERT_EQ(rc, -1);
    tls_assert_failed_closed(utest_result, t);
    kl_tls_mbedtls_ctx_destroy(cctx);
    tls_peer_join(&p);
}

/* Unknown CA fails closed: the client trusts CA2 but the server presents a leaf
 * signed by CA1, so chain verification fails. No plaintext fallback.
 * REVERT PROOF: trust HL_TLS_CA1_PEM and it passes. */
UTEST(smtp_tls, unknown_ca_fails_closed)
{
    TlsPeer p; ASSERT_EQ(tls_peer_setup(&p), 0);
    ASSERT_EQ(pthread_create(&p.tid, NULL, tls_peer_thread, &p), 0);

    void (*prev)(int) = signal(SIGALRM, tls_watchdog_fired); alarm(30);

    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", p.port, 10000,
                                                   NULL, NULL, NULL, NULL);
    ASSERT_TRUE(t != NULL);
    KlTlsCtx *cctx = NULL;
    KlTlsConfig cfg = tls_client_cfg(&cctx, HL_TLS_CA2_PEM, sizeof HL_TLS_CA2_PEM);
    int rc = hl_smtp_transport_implicit_tls(t, "localhost", &cfg, 10000);

    alarm(0); signal(SIGALRM, prev);

    ASSERT_EQ(rc, -1);
    tls_assert_failed_closed(utest_result, t);
    kl_tls_mbedtls_ctx_destroy(cctx);
    tls_peer_join(&p);
}

/* The TLS handshake respects the post-resolution operation deadline (Dop): the
 * peer accepts but never answers the ClientHello, so the client handshake pump
 * must terminate at Dop, NOT at implicit_tls's own (much larger) stage timeout.
 * Connect with a 600 ms Dop; call implicit_tls with a 10 s stage budget; the
 * handshake must fail in well under that.
 * REVERT PROOF: if the pump did not clamp the stage to Dop, this would run ~10 s
 * (the stage budget) and the elapsed bound below would fail. */
UTEST(smtp_tls, handshake_timeout_respects_dop)
{
    TlsPeer p; ASSERT_EQ(tls_peer_setup(&p), 0);
    p.stall_handshake = 1;
    ASSERT_EQ(pthread_create(&p.tid, NULL, tls_peer_thread, &p), 0);

    void (*prev)(int) = signal(SIGALRM, tls_watchdog_fired); alarm(30);

    /* 600 ms connect timeout -> Dop = connect + 600 ms. */
    HlSmtpTransport *t = hl_smtp_transport_connect("127.0.0.1", p.port, 600,
                                                   NULL, NULL, NULL, NULL);
    ASSERT_TRUE(t != NULL);
    KlTlsCtx *cctx = NULL;
    KlTlsConfig cfg = tls_client_cfg(&cctx, HL_TLS_CA1_PEM, sizeof HL_TLS_CA1_PEM);

    uint64_t t0 = kl_monotonic_ms();
    int rc = hl_smtp_transport_implicit_tls(t, "localhost", &cfg, 10000); /* 10 s stage */
    uint64_t elapsed = kl_monotonic_ms() - t0;

    alarm(0); signal(SIGALRM, prev);

    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(elapsed < 3000);          /* bounded by the 600 ms Dop, not the 10 s stage */
    tls_assert_failed_closed(utest_result, t);
    kl_tls_mbedtls_ctx_destroy(cctx);
    tls_peer_join(&p);
    ASSERT_EQ(p.reached_handshake, 1);    /* the client did send its ClientHello */
}

UTEST_MAIN()
