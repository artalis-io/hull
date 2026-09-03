/**
 * @file cap/smtp_op.h
 * @brief Owned, deep-copied SMTP operation inputs.
 *
 * Model 2 runs the SMTP conversation on a worker thread after the runtime
 * suspends. The runtime bindings borrow Lua / QuickJS string and array storage,
 * which the interpreter may move or collect once suspended, so the worker must
 * not reference any of it. #HlSmtpOp is an independent heap copy of every field
 * the operation needs; credentials and the copied body are volatile-scrubbed on
 * free. See docs/smtp_keel_slice2c_plan.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_OP_H
#define HL_CAP_SMTP_OP_H

#include "hull/cap/smtp.h"   /* HlSmtpMessage */

typedef struct HlSmtpOp {
    char  *host;
    int    port;
    int    use_tls;          /* 0 = none, 1 = STARTTLS, 2 = implicit TLS */
    char  *username;         /* NULL = no auth; scrubbed on free */
    char  *password;         /* NULL = no auth; scrubbed on free */
    char  *from;
    char  *to;
    char **cc;               /* owned vector of cc_count owned strings (NULL = none) */
    int    cc_count;
    char  *reply_to;         /* NULL = omit */
    char  *subject;
    char  *body;             /* scrubbed on free */
    char  *content_type;     /* NULL = default */
    int    timeout_ms;
} HlSmtpOp;

/**
 * Deep-copy @p msg (plus the resolved @p timeout_ms) into a newly allocated op
 * that solely owns every field. Returns NULL on invalid input or OOM; on OOM
 * any partial copies are scrubbed + freed. Intended to run on the event-loop
 * thread at submit time, while the borrowed source storage is still valid.
 */
HlSmtpOp *hl_smtp_op_create(const HlSmtpMessage *msg, int timeout_ms);

/**
 * Scrub credentials + body (volatile memset) and free the op and all owned
 * storage. NULL-safe. Safe to call on any thread that owns the op.
 */
void hl_smtp_op_free(HlSmtpOp *op);

/**
 * Fill @p out with a borrowed #HlSmtpMessage view over the op's owned storage,
 * so hl_smtp_execute can consume it unchanged. The view is valid only until
 * hl_smtp_op_free(op).
 */
void hl_smtp_op_message(const HlSmtpOp *op, HlSmtpMessage *out);

#endif /* HL_CAP_SMTP_OP_H */
