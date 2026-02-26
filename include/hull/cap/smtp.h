/*
 * cap/smtp.h — SMTP email capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_H
#define HL_CAP_SMTP_H

typedef struct {
    const char *host;
    int         port;
    const char *username;
    const char *password;
    int         use_tls;
} HlSmtpConfig;

typedef struct {
    const char *from;
    const char *to;
    const char *subject;
    const char *body;
    const char *content_type;
} HlSmtpMessage;

int hl_cap_smtp_send(const HlSmtpConfig *cfg,
                       const HlSmtpMessage *msg);

#endif /* HL_CAP_SMTP_H */
