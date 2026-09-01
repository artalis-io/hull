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

#include "hull/cap/smtp_op.h"

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
    s->host = dupz("mail.example.com"); s->user = dupz("u@example.com");
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
    ASSERT_STREQ(op->username, "u@example.com");
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

UTEST_MAIN();
