/*
 * smtp_op.c - deep-copy + scrub the owned SMTP operation inputs.
 *
 * See include/hull/cap/smtp_op.h and docs/smtp_keel_slice2c_plan.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/smtp_op.h"

#include <stdlib.h>
#include <string.h>

/* Volatile-memset scrub, mirroring smtp.c's smtp_secure_zero (static per file):
 * the store is through a volatile pointer so the compiler cannot elide it. */
static void smtp_op_secure_zero(void *p, size_t n)
{
    if (!p || n == 0)
        return;
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--)
        *v++ = 0;
}

/* strdup that maps NULL -> NULL (an omitted optional field stays omitted). */
static char *dup_str(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *c = malloc(n + 1);
    if (!c)
        return NULL;
    memcpy(c, s, n + 1);
    return c;
}

/* Scrub the sensitive fields and free the op + all owned storage. Shared by the
 * OOM path in create and by hl_smtp_op_free, so every exit scrubs identically. */
static void op_destroy(HlSmtpOp *op)
{
    if (!op)
        return;
    if (op->username)
        smtp_op_secure_zero(op->username, strlen(op->username));
    if (op->password)
        smtp_op_secure_zero(op->password, strlen(op->password));
    if (op->body)
        smtp_op_secure_zero(op->body, strlen(op->body));
    free(op->username);
    free(op->password);
    free(op->body);
    free(op->host);
    free(op->from);
    free(op->to);
    free(op->reply_to);
    free(op->subject);
    free(op->content_type);
    if (op->cc) {
        for (int i = 0; i < op->cc_count; i++)
            free(op->cc[i]);   /* free(NULL) is safe for unfilled slots */
        free(op->cc);
    }
    free(op);
}

void hl_smtp_op_free(HlSmtpOp *op)
{
    op_destroy(op);
}

HlSmtpOp *hl_smtp_op_create(const HlSmtpMessage *msg, int timeout_ms)
{
    if (!msg)
        return NULL;

    HlSmtpOp *op = calloc(1, sizeof *op);
    if (!op)
        return NULL;

    op->port       = msg->port;
    op->use_tls    = msg->use_tls;
    op->timeout_ms = timeout_ms;

    /* Copy each non-NULL field; any failure is OOM -> scrub + free + NULL. */
#define CP(field) do {                                        \
        if (msg->field) {                                     \
            op->field = dup_str(msg->field);                  \
            if (!op->field) goto oom;                         \
        }                                                     \
    } while (0)
    CP(host);
    CP(username);
    CP(password);
    CP(from);
    CP(to);
    CP(reply_to);
    CP(subject);
    CP(body);
    CP(content_type);
#undef CP

    if (msg->cc && msg->cc_count > 0) {
        op->cc = calloc((size_t)msg->cc_count, sizeof(char *));
        if (!op->cc)
            goto oom;
        /* Set the count BEFORE filling so op_destroy frees every slot (NULL
         * slots are free(NULL)-safe) if a mid-vector copy OOMs. */
        op->cc_count = msg->cc_count;
        for (int i = 0; i < msg->cc_count; i++) {
            if (msg->cc[i]) {
                op->cc[i] = dup_str(msg->cc[i]);
                if (!op->cc[i])
                    goto oom;
            }
        }
    }

    return op;

oom:
    op_destroy(op);
    return NULL;
}

void hl_smtp_op_message(const HlSmtpOp *op, HlSmtpMessage *out)
{
    memset(out, 0, sizeof *out);
    out->host         = op->host;
    out->port         = op->port;
    out->use_tls      = op->use_tls;
    out->username     = op->username;
    out->password     = op->password;
    out->from         = op->from;
    out->to           = op->to;
    /* op->cc (char**) is exposed read-only as const char**; launder through
     * void* so -Wcast-qual does not flag the added-const intermediate. */
    out->cc           = (const char **)(void *)op->cc;
    out->cc_count     = op->cc_count;
    out->reply_to     = op->reply_to;
    out->subject      = op->subject;
    out->body         = op->body;
    out->content_type = op->content_type;
}
