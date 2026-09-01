/*
 * test_smtp_op.c - HlSmtpOp deep-copy + scrub.
 *
 * Proves the op owns independent copies of every field, so it survives the
 * borrowed Lua/QuickJS source storage being reused/freed after the runtime
 * suspends. Scrub-on-free is exercised (verified clean under ASan/MSan; a
 * post-free content assertion would be a use-after-free).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

/* Direct-source include (compiled with -DHL_SMTP_TEST_HOOKS via the Makefile
 * rule) so the fail-after-N sweep can drive smtp_op.c's allocation seam and
 * interpose free. cap_smtp_op.o is excluded from this test's link. */
#include "../../../src/hull/cap/smtp_op.c"

#include <stdlib.h>
#include <string.h>

/* Build a message whose strings live in freshly malloc'd buffers, so a test can
 * scribble + free them and prove the op copied rather than borrowed. */
typedef struct {
    char *host, *user, *pass, *from, *to, *cc0, *cc1, *reply, *subj, *body, *ct;
    const char *cc[2];
    HlSmtpMessage msg;
} Src;

static char *dupz(const char *s) { char *c = malloc(strlen(s) + 1); strcpy(c, s); return c; }

static void src_init(Src *s)
{
    s->host = dupz("mail.example.com"); s->user = dupz("s3cr3t-user@example.com");
    s->pass = dupz("s3cr3t-pw"); s->from = dupz("from@example.com");
    s->to = dupz("to@example.com"); s->cc0 = dupz("cc0@example.com");
    s->cc1 = dupz("cc1@example.com"); s->reply = dupz("reply@example.com");
    s->subj = dupz("Subject line"); s->body = dupz("Body with s3cr3t content");
    s->ct = dupz("text/html");
    s->cc[0] = s->cc0; s->cc[1] = s->cc1;
    memset(&s->msg, 0, sizeof s->msg);
    s->msg.host = s->host; s->msg.port = 587; s->msg.use_tls = 1;
    s->msg.username = s->user; s->msg.password = s->pass;
    s->msg.from = s->from; s->msg.to = s->to;
    s->msg.cc = s->cc; s->msg.cc_count = 2;
    s->msg.reply_to = s->reply; s->msg.subject = s->subj;
    s->msg.body = s->body; s->msg.content_type = s->ct;
}

/* Overwrite + free every source buffer, simulating the interpreter reusing the
 * borrowed storage after the runtime suspends. */
static void src_scribble_and_free(Src *s)
{
    char *all[] = { s->host, s->user, s->pass, s->from, s->to, s->cc0, s->cc1,
                    s->reply, s->subj, s->body, s->ct };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        memset(all[i], 'X', strlen(all[i]));
        free(all[i]);
    }
}

UTEST(smtp_op, deep_copy_is_independent)
{
    Src s; src_init(&s);
    HlSmtpOp *op = hl_smtp_op_create(&s.msg, 9000);
    ASSERT_TRUE(op != NULL);

    src_scribble_and_free(&s);   /* the borrow is now gone */

    ASSERT_STREQ(op->host, "mail.example.com");
    ASSERT_EQ(op->port, 587);
    ASSERT_EQ(op->use_tls, 1);
    ASSERT_STREQ(op->username, "s3cr3t-user@example.com");
    ASSERT_STREQ(op->password, "s3cr3t-pw");
    ASSERT_STREQ(op->from, "from@example.com");
    ASSERT_STREQ(op->to, "to@example.com");
    ASSERT_EQ(op->cc_count, 2);
    ASSERT_STREQ(op->cc[0], "cc0@example.com");
    ASSERT_STREQ(op->cc[1], "cc1@example.com");
    ASSERT_STREQ(op->reply_to, "reply@example.com");
    ASSERT_STREQ(op->subject, "Subject line");
    ASSERT_STREQ(op->body, "Body with s3cr3t content");
    ASSERT_STREQ(op->content_type, "text/html");
    ASSERT_EQ(op->timeout_ms, 9000);

    hl_smtp_op_free(op);   /* scrubs creds + body; ASan/MSan verify clean */
}

UTEST(smtp_op, message_view_roundtrips)
{
    Src s; src_init(&s);
    HlSmtpOp *op = hl_smtp_op_create(&s.msg, 1000);
    ASSERT_TRUE(op != NULL);

    HlSmtpMessage v;
    hl_smtp_op_message(op, &v);
    ASSERT_STREQ(v.host, op->host);
    ASSERT_EQ(v.port, op->port);
    ASSERT_EQ(v.use_tls, op->use_tls);
    ASSERT_STREQ(v.username, op->username);
    ASSERT_STREQ(v.password, op->password);
    ASSERT_STREQ(v.from, op->from);
    ASSERT_STREQ(v.to, op->to);
    ASSERT_EQ(v.cc_count, 2);
    ASSERT_STREQ(v.cc[0], "cc0@example.com");
    ASSERT_STREQ(v.subject, op->subject);
    ASSERT_STREQ(v.body, op->body);
    ASSERT_STREQ(v.content_type, op->content_type);

    src_scribble_and_free(&s);
    hl_smtp_op_free(op);
}

UTEST(smtp_op, null_optionals_ok)
{
    HlSmtpMessage msg;
    memset(&msg, 0, sizeof msg);
    msg.host = "h"; msg.port = 25; msg.use_tls = 0;
    msg.from = "f"; msg.to = "t"; msg.subject = "s"; msg.body = "b";
    /* username/password/cc/reply_to/content_type all NULL */

    HlSmtpOp *op = hl_smtp_op_create(&msg, 500);
    ASSERT_TRUE(op != NULL);
    ASSERT_TRUE(op->username == NULL);
    ASSERT_TRUE(op->password == NULL);
    ASSERT_TRUE(op->cc == NULL);
    ASSERT_EQ(op->cc_count, 0);
    ASSERT_TRUE(op->reply_to == NULL);
    ASSERT_TRUE(op->content_type == NULL);
    ASSERT_STREQ(op->host, "h");

    HlSmtpMessage v;
    hl_smtp_op_message(op, &v);
    ASSERT_TRUE(v.cc == NULL);
    ASSERT_EQ(v.cc_count, 0);

    hl_smtp_op_free(op);
}

UTEST(smtp_op, free_null_safe)
{
    hl_smtp_op_free(NULL);   /* must not crash */
    ASSERT_TRUE(1);
}

UTEST(smtp_op, create_null_msg_returns_null)
{
    ASSERT_TRUE(hl_smtp_op_create(NULL, 1000) == NULL);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Deterministic fail-after-N allocation sweep.
 *
 * A fail-after-N allocation injector + a free interposer over smtp_op.c's
 * allocation seam. The injector fails the g_fail_at-th op allocation; the free
 * interposer snapshots each freed buffer's bytes-at-free into g_released, so a
 * scrubbed credential/body reads all-zero and a plaintext secret does not.
 * ──────────────────────────────────────────────────────────────────────── */
#define MAXLIVE 64
static struct { void *p; size_t n; } g_live[MAXLIVE];
static int    g_live_n;
static int    g_alloc_calls;   /* attempted op allocations this run */
static int    g_fail_at;       /* fail this Nth op allocation (0 = never) */
static char   g_released[1 << 16];
static size_t g_released_n;

static void inj_reset(int fail_at)
{
    g_live_n = 0; g_alloc_calls = 0; g_fail_at = fail_at; g_released_n = 0;
    memset(g_released, 0, sizeof g_released);
}

static void *inj_alloc(size_t n)
{
    g_alloc_calls++;
    if (g_fail_at && g_alloc_calls == g_fail_at)
        return NULL;   /* deterministic injected failure */
    void *p = malloc(n);
    if (p && g_live_n < MAXLIVE) {
        g_live[g_live_n].p = p;
        g_live[g_live_n].n = n;
        g_live_n++;
    }
    return p;
}

static void inj_free(void *p)
{
    if (!p) { return; }
    for (int i = 0; i < g_live_n; i++) {
        if (g_live[i].p == p) {
            size_t n = g_live[i].n;   /* snapshot content-at-free */
            if (g_released_n + n <= sizeof g_released) {
                memcpy(g_released + g_released_n, p, n);
                g_released_n += n;
            }
            g_live[i] = g_live[--g_live_n];
            break;
        }
    }
    free(p);
}

/* Was `needle` present in ANY freed buffer at free time? (g_released is a concat
 * of possibly-NUL-embedded buffers, so scan bytewise.) */
static int released_contains(const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0 || g_released_n < nl)
        return 0;
    for (size_t i = 0; i + nl <= g_released_n; i++)
        if (memcmp(g_released + i, needle, nl) == 0)
            return 1;
    return 0;
}

/* "s3cr3t" is seeded into the username, password, and body (the three fields
 * scrubbed on free) and nowhere else, so finding it in a freed buffer means a
 * credential or the body was released unscrubbed. */
#define SECRET "s3cr3t"

UTEST(smtp_op, oom_sweep_cleans_and_scrubs)
{
    Src s; src_init(&s);

    smtp_op_test_alloc = inj_alloc;
    smtp_op_test_free  = inj_free;

    /* A clean run establishes the total allocation count and proves the
     * success-path free also scrubs (no SECRET in released bytes). */
    inj_reset(0);
    HlSmtpOp *ok = hl_smtp_op_create(&s.msg, 1234);
    ASSERT_TRUE(ok != NULL);
    int total = g_alloc_calls;
    ASSERT_TRUE(total > 1);
    hl_smtp_op_free(ok);
    ASSERT_EQ(g_live_n, 0);
    ASSERT_FALSE(released_contains(SECRET));

    /* Fail each allocation in turn; every injected failure must clean up fully
     * and scrub, leave no partial op, and never touch the source. */
    for (int n = 1; n <= total; n++) {
        inj_reset(n);
        HlSmtpOp *op = hl_smtp_op_create(&s.msg, 1234);

        ASSERT_TRUE(op == NULL);                   /* construction returns failure */
        ASSERT_EQ(g_live_n, 0);                    /* earlier allocations all freed; no partial op escapes */
        ASSERT_FALSE(released_contains(SECRET));    /* credentials + body scrubbed before release */

        /* Source inputs remain untouched (create only reads them). */
        ASSERT_STREQ(s.host, "mail.example.com");
        ASSERT_STREQ(s.user, "s3cr3t-user@example.com");
        ASSERT_STREQ(s.pass, "s3cr3t-pw");
        ASSERT_STREQ(s.body, "Body with s3cr3t content");
        ASSERT_STREQ(s.cc0, "cc0@example.com");
        ASSERT_STREQ(s.cc1, "cc1@example.com");
        ASSERT_EQ(s.msg.cc_count, 2);
    }

    smtp_op_test_alloc = 0;
    smtp_op_test_free  = 0;
    src_scribble_and_free(&s);   /* source buffers are libc-owned (dupz), not tracked */
}

UTEST_MAIN();
