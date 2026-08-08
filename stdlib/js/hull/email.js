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
 * API providers use `httpClient.async.post` so they cooperate with the event
 * loop. SMTP uses the C `smtp.send` cap (mbedTLS + embedded Mozilla CA
 * bundle).
 *
 * Errors: `email.send` follows the stdlib error convention
 * (docs/stdlib_style.md section 1) — a failure to send THROWS an `Error` with a
 * stable `.code`; success resolves to `true`. Callers that branch use
 * try/catch:
 *
 *     try { await email.send(opts); }
 *     catch (e) { log.error(String(e));    // e.code is stable
 *         if (e.code === "delivery_failed") { ... } }
 *
 * Codes: `invalid_argument` (missing/invalid opts, from, to, subject, body, or a
 * provider's required apiKey), `unknown_provider`, `delivery_failed` (the
 * provider attempted delivery and it failed). Composes with `hull/jobs`: a
 * `send_email` handler that calls `email.send(job.data)` retries on a throw.
 *
 * @license AGPL-3.0-or-later
 */

import { smtp } from "hull:smtp";
import { httpClient } from "hull:http-client";
import { json } from "hull:json";

const email = {};

// Throw a coded Error. e.code is the stable, branch-on identity
// (docs/stdlib_style.md section 1).
function emailError(code, message) {
    const e = new Error(message);
    e.code = code;
    throw e;
}

// Provider adapters: each takes (opts), returns true on success, throws on
// failure (delivery_failed / invalid_argument).
const providers = {};

providers.smtp = function(opts) {
    // smtp.send is a thin C-cap binding that keeps its own {ok,error} shape;
    // translate a failure (thrown or {ok:false}) into the email.send throw.
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
        emailError("delivery_failed", "smtp: " + String(e));
    }
    if (!result || !result.ok) {
        emailError("delivery_failed",
            "smtp: " + ((result && result.error) || "send failed"));
    }
    return true;
};

providers.postmark = async function(opts) {
    if (!opts.api_key)
        emailError("invalid_argument", "postmark: api_key required");

    const payload = {
        From: opts.from,
        To: opts.to,
        Subject: opts.subject,
    };
    // Accept either array or string for `cc`.
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

    let resp;
    try {
        resp = await httpClient.async.post(
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
        emailError("delivery_failed", "postmark: " + String(e));
    }
    if (!resp)
        emailError("delivery_failed", "postmark: no response");
    if (resp.status >= 200 && resp.status < 300)
        return true;
    emailError("delivery_failed", "postmark: " + (resp.body || "unknown error"));
};

providers.sendgrid = async function(opts) {
    if (!opts.api_key)
        emailError("invalid_argument", "sendgrid: api_key required");

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
        resp = await httpClient.async.post(
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
        emailError("delivery_failed", "sendgrid: " + String(e));
    }
    if (!resp)
        emailError("delivery_failed", "sendgrid: no response");
    if (resp.status >= 200 && resp.status < 300)
        return true;
    emailError("delivery_failed", "sendgrid: " + (resp.body || "unknown error"));
};

providers.resend = async function(opts) {
    if (!opts.api_key)
        emailError("invalid_argument", "resend: api_key required");

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
    // Resend accepts cc as an array of strings; accept a single string too.
    if (Array.isArray(opts.cc)) payload.cc = opts.cc;
    else if (typeof opts.cc === "string") payload.cc = [opts.cc];

    let resp;
    try {
        resp = await httpClient.async.post(
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
        emailError("delivery_failed", "resend: " + String(e));
    }
    if (!resp)
        emailError("delivery_failed", "resend: no response");
    if (resp.status >= 200 && resp.status < 300)
        return true;
    emailError("delivery_failed", "resend: " + (resp.body || "unknown error"));
};

const ADDR_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

/**
 * Send an email via the selected provider.
 *
 * Validates `from`/`to` against a simple `local@domain.tld` pattern before
 * dispatch. THROWS an `Error` with a stable `.code` on any failure; resolves to
 * `true` on success. See the module header for the code list.
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
 * @returns {Promise<true>}  resolves true on success; throws {code} on failure.
 *
 * @example
 * try {
 *     await email.send({
 *         provider: "postmark",
 *         from: "app@example.com", to: "user@example.com",
 *         subject: "Hi", body: "Hello!",
 *         api_key: env.get("POSTMARK_TOKEN"),
 *     });
 * } catch (e) { log.error(String(e)); }
 */
email.send = async function(opts) {
    if (!opts) emailError("invalid_argument", "opts required");
    if (!opts.from) emailError("invalid_argument", "from required");
    if (!opts.to) emailError("invalid_argument", "to required");
    if (!opts.subject) emailError("invalid_argument", "subject required");
    if (!opts.body) emailError("invalid_argument", "body required");

    // Basic email format validation (parity with the Lua module).
    if (!ADDR_RE.test(opts.from))
        emailError("invalid_argument", "invalid from address");
    if (!ADDR_RE.test(opts.to))
        emailError("invalid_argument", "invalid to address");

    const provider = opts.provider || "smtp";
    const fn = providers[provider];
    if (!fn)
        emailError("unknown_provider", "unknown provider: " + String(provider));
    return await fn(opts);
};

export { email };
