// hull:email — Outbound email with provider dispatch
//
// Supports direct SMTP and API providers (Postmark, SendGrid, Resend).
// SMTP goes through the C smtp.send() binding; API providers use http.async.post()
// (non-blocking).
//
// Usage:
//   import { email } from "hull:email";
//   const result = email.send({
//       provider: "smtp",            // or "postmark", "sendgrid", "resend"
//       from: "app@example.com",
//       to: "user@example.com",
//       subject: "Hello",
//       body: "Message body",
//       // SMTP-specific:
//       smtp_host: "smtp.example.com",
//       smtp_port: 587,
//       smtp_user: "apikey",
//       smtp_pass: env.get("SMTP_PASS"),
//       smtp_tls: true,
//       // API-specific:
//       api_key: env.get("POSTMARK_TOKEN"),
//       // Optional:
//       cc: ["admin@example.com"],
//       reply_to: "support@example.com",
//       content_type: "text/html",
//   });
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { smtp } from "hull:smtp";
import { http } from "hull:http";
import { json } from "hull:json";

const email = {};

const providers = {};

providers.smtp = function(opts) {
    return smtp.send({
        host: opts.smtp_host,
        port: opts.smtp_port || 587,
        username: opts.smtp_user,
        password: opts.smtp_pass,
        tls: opts.smtp_tls !== false,
        from: opts.from,
        to: opts.to,
        cc: opts.cc,
        reply_to: opts.reply_to,
        subject: opts.subject,
        body: opts.body,
        content_type: opts.content_type || "text/plain",
    });
};

providers.postmark = async function(opts) {
    if (!opts.api_key)
        return { ok: false, error: "postmark: api_key required" };

    const payload = {
        From: opts.from,
        To: opts.to,
        Subject: opts.subject,
    };
    // M-3: accept either array or string for `cc`.
    if (Array.isArray(opts.cc))
        payload.Cc = opts.cc.join(",");
    else if (typeof opts.cc === "string")
        payload.Cc = opts.cc;
    if (opts.reply_to)
        payload.ReplyTo = opts.reply_to;
    if (opts.content_type === "text/html")
        payload.HtmlBody = opts.body;
    else
        payload.TextBody = opts.body;

    // M-2: trap network exceptions and return the documented {ok, error}
    // contract rather than throwing out of email.send().
    let resp;
    try {
        resp = await http.async.post(
            "https://api.postmarkapp.com/email",
            json.encode(payload),
            {
                headers: {
                    "X-Postmark-Server-Token": opts.api_key,
                    "Content-Type": "application/json",
                    "Accept": "application/json",
                },
            }
        );
    } catch (e) {
        return { ok: false, error: "postmark: " + String(e) };
    }
    if (!resp)
        return { ok: false, error: "postmark: no response" };
    if (resp.status >= 200 && resp.status < 300)
        return { ok: true };
    return { ok: false, error: "postmark: " + (resp.body || "unknown error") };
};

providers.sendgrid = async function(opts) {
    if (!opts.api_key)
        return { ok: false, error: "sendgrid: api_key required" };

    const payload = {
        personalizations: [{ to: [{ email: opts.to }] }],
        from: { email: opts.from },
        subject: opts.subject,
        content: [{
            type: opts.content_type || "text/plain",
            value: opts.body,
        }],
    };
    if (opts.reply_to)
        payload.reply_to = { email: opts.reply_to };

    let resp;
    try {
        resp = await http.async.post(
            "https://api.sendgrid.com/v3/mail/send",
            json.encode(payload),
            {
                headers: {
                    "Authorization": "Bearer " + opts.api_key,
                    "Content-Type": "application/json",
                },
            }
        );
    } catch (e) {
        return { ok: false, error: "sendgrid: " + String(e) };
    }
    if (!resp)
        return { ok: false, error: "sendgrid: no response" };
    if (resp.status >= 200 && resp.status < 300)
        return { ok: true };
    return { ok: false, error: "sendgrid: " + (resp.body || "unknown error") };
};

providers.resend = async function(opts) {
    if (!opts.api_key)
        return { ok: false, error: "resend: api_key required" };

    const payload = {
        from: opts.from,
        to: opts.to,
        subject: opts.subject,
    };
    if (opts.content_type === "text/html")
        payload.html = opts.body;
    else
        payload.text = opts.body;
    if (opts.reply_to) payload.reply_to = opts.reply_to;
    // M-3: Resend accepts cc as array of strings, but accept a single
    // string too.
    if (Array.isArray(opts.cc)) payload.cc = opts.cc;
    else if (typeof opts.cc === "string") payload.cc = [opts.cc];

    let resp;
    try {
        resp = await http.async.post(
            "https://api.resend.com/emails",
            json.encode(payload),
            {
                headers: {
                    "Authorization": "Bearer " + opts.api_key,
                    "Content-Type": "application/json",
                },
            }
        );
    } catch (e) {
        return { ok: false, error: "resend: " + String(e) };
    }
    if (!resp)
        return { ok: false, error: "resend: no response" };
    if (resp.status >= 200 && resp.status < 300)
        return { ok: true };
    return { ok: false, error: "resend: " + (resp.body || "unknown error") };
};

email.send = async function(opts) {
    if (!opts) return { ok: false, error: "opts required" };
    if (!opts.from) return { ok: false, error: "from required" };
    if (!opts.to) return { ok: false, error: "to required" };
    if (!opts.subject) return { ok: false, error: "subject required" };
    if (!opts.body) return { ok: false, error: "body required" };

    const provider = opts.provider || "smtp";
    const fn = providers[provider];
    if (!fn)
        return { ok: false, error: "unknown provider: " + String(provider) };
    return await fn(opts);
};

export { email };
