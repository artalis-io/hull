/**
 * @file hull:email
 * @module hull:email
 * @description Outbound email with pluggable provider dispatch. Lua parity:
 *   `hull.email`.
 *
 * Wraps four delivery backends behind one `email.send(opts)` call:
 *
 *   - `"smtp"`     — direct SMTP via the C `smtp.send` binding.
 *   - `"postmark"` — Postmark HTTPS API.
 *   - `"sendgrid"` — SendGrid HTTPS API.
 *   - `"resend"`   — Resend HTTPS API.
 *
 * API providers use `http.async.post` so they cooperate with the event
 * loop. SMTP uses the C `smtp.send` cap (mbedTLS + embedded Mozilla CA
 * bundle).
 *
 * @license AGPL-3.0-or-later
 */

import { smtp } from "hull:smtp";
import { http } from "hull:http-client";
import { json } from "hull:json";

const email = {};

const providers = {};

providers.smtp = function(opts) {
    // Phase 6 audit M-3: mirror the three API providers — wrap in
    // try/catch so the {ok, error} contract is uniform across providers.
    let result;
    try {
        result = smtp.send({
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
    } catch (e) {
        return { ok: false, error: "smtp: " + String(e) };
    }
    return result;
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

/**
 * Send an email via the selected provider.
 *
 * @param {Object} opts
 * @param {("smtp"|"postmark"|"sendgrid"|"resend")} [opts.provider="smtp"]
 * @param {string} opts.from
 * @param {string} opts.to
 * @param {string} opts.subject
 * @param {string} opts.body
 * @param {string[]} [opts.cc]
 * @param {string}   [opts.reply_to]
 * @param {("text/plain"|"text/html")} [opts.content_type="text/plain"]
 * @param {string}   [opts.api_key]    Required for API providers.
 * @param {string}   [opts.smtp_host]
 * @param {number}   [opts.smtp_port=587]
 * @param {string}   [opts.smtp_user]
 * @param {string}   [opts.smtp_pass]
 * @param {boolean}  [opts.smtp_tls=true]
 * @returns {Promise<{ok:true} | {ok:false, error:string}>}
 *
 * @example
 * const r = await email.send({
 *     provider: "postmark",
 *     from: "app@example.com", to: "user@example.com",
 *     subject: "Hi", body: "Hello!",
 *     api_key: env.get("POSTMARK_TOKEN"),
 * });
 * if (!r.ok) log.error(r.error);
 */
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
