/**
 * @file hull:email
 * @module hull:email
 * @description Outbound email with pluggable provider dispatch. Lua parity:
 *   `hull.email`.
 *
 * Wraps four delivery backends behind one `email.send(opts)` call:
 *
 *   - `"smtp"`     - direct SMTP via the C `smtp.send` binding.
 *   - `"postmark"` - Postmark HTTPS API.
 *   - `"sendgrid"` - SendGrid HTTPS API.
 *   - `"resend"`   - Resend HTTPS API.
 *
 * API providers use `httpClient.async.post` so they cooperate with the event
 * loop. SMTP uses the C `smtp.send` cap (mbedTLS + embedded Mozilla CA
 * bundle).
 *
 * Errors: `email.send` follows the stdlib error convention
 * (docs/stdlib_style.md section 1) - a failure to send THROWS an `Error` with a
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
import { _refusal } from "hull:_refusal";
import { _text } from "hull:_text";

const email = {};

// Throw a coded Error. e.code is the stable, branch-on identity
// (docs/stdlib_style.md section 1).
function emailError(code, message) {
    const e = new Error(message);
    e.code = code;
    throw e;
}

// A provider's error reply as error text: capped and made storable (audit
// 39; see the Lua sibling).
const PROVIDER_ERROR_MAX = 1024;
function providerError(resp) {
    const b = resp ? resp.body : undefined;
    if (b === undefined || b === null || b === "") return "unknown error";
    return _text.storable(b, PROVIDER_ERROR_MAX, true);
}

// Provider adapters: each takes (opts), returns true on success, throws on
// failure (delivery_failed / invalid_argument).
// No prototype: `provider` comes from config or env, and on a plain {}
// "toString" / "constructor" resolved to Object.prototype's functions - the
// send "succeeded" and nothing went out.
const providers = Object.create(null);

// An address: at most 254 BYTES of UTF-8 (RFC 5321's path limit), checked
// before the pattern - as the Lua twin counts it. In UTF-16 units a ~250-
// character CJK address (~750 bytes) passed (audit 21). An unpaired
// surrogate has no UTF-8 form (QuickJS writes it as invalid bytes, which the
// Lua twin's UTF-8 check refuses), so it makes the length Infinity: refused
// (audit 28).
const MAX_ADDR = 254;
function utf8Length(s) {
    let n = 0;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length &&
                 (s.charCodeAt(i + 1) & 0xfc00) === 0xdc00) { n += 4; i++; }
        else if (c >= 0xd800 && c <= 0xdfff) return Infinity;
        else n += 3;
    }
    return n;
}
function addrOk(a) {
    return typeof a === "string" && a.length <= MAX_ADDR &&
        utf8Length(a) <= MAX_ADDR && ADDR_RE.test(a);
}

// opts.cc as a list of strings; a string is one recipient.
function ccList(cc) {
    if (cc === undefined || cc === null) return null;
    const list = typeof cc === "string" ? [cc] : cc;
    if (!Array.isArray(list))
        emailError("invalid_argument", "cc must be a string or a list of strings");
    for (const a of list)
        if (!addrOk(a)) emailError("invalid_argument", "invalid cc address");
    return list.length > 0 ? list.slice() : null;
}

providers.smtp = async function(opts) {
    // smtp.send returns a Promise resolving to {ok,error} (model 2: it yields to
    // the event loop while the worker runs the SMTP conversation). Translate a
    // failure (thrown or {ok:false}) into the email.send throw.
    let result;
    try {
        result = await smtp.send({
            host: opts.smtp_host,
            port: opts.smtp_port || 587,
            username: opts.smtp_user,
            password: opts.smtp_pass,
            tls: opts.smtp_tls !== false,
            from: opts.from,
            to: opts.to,
            cc: ccList(opts.cc) || undefined,
            reply_to: opts.reply_to,
            subject: opts.subject,
            body: opts.body,
            content_type: opts.content_type || "text/plain",
        });
    } catch (e) {
        // The runtime refusing the send (busy: the admission cap or the
        // off-heap copy total; wait_refused) attempted no delivery: re-thrown
        // with its code, as Lua's smtp.send raise reaches the caller (audit 36).
        if (_refusal.code(e)) throw e;
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
    const cc = ccList(opts.cc);
    if (cc) payload.Cc = cc.join(",");
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
        // A runtime refusal attempted no delivery: re-thrown with its code
        // (audit 39), as the smtp provider does.
        if (_refusal.code(e)) throw e;
        emailError("delivery_failed", "postmark: " + String(e));
    }
    if (!resp)
        emailError("delivery_failed", "postmark: no response");
    if (resp.status >= 200 && resp.status < 300)
        return true;
    emailError("delivery_failed", "postmark: " + providerError(resp));
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
    // cc rides in the personalization; it used to be dropped silently.
    const cc = ccList(opts.cc);
    if (cc) payload.personalizations[0].cc = cc.map((email) => ({ email }));

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
        // A runtime refusal attempted no delivery: re-thrown with its code
        // (audit 39), as the smtp provider does.
        if (_refusal.code(e)) throw e;
        emailError("delivery_failed", "sendgrid: " + String(e));
    }
    if (!resp)
        emailError("delivery_failed", "sendgrid: no response");
    if (resp.status >= 200 && resp.status < 300)
        return true;
    emailError("delivery_failed", "sendgrid: " + providerError(resp));
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
    const cc = ccList(opts.cc);
    if (cc) payload.cc = cc;

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
        // A runtime refusal attempted no delivery: re-thrown with its code
        // (audit 39), as the smtp provider does.
        if (_refusal.code(e)) throw e;
        emailError("delivery_failed", "resend: " + String(e));
    }
    if (!resp)
        emailError("delivery_failed", "resend: no response");
    if (resp.status >= 200 && resp.status < 300)
        return true;
    emailError("delivery_failed", "resend: " + providerError(resp));
};

// One address: no control character, and none of the characters that
// separate or quote addresses ("a@x.co,b" passed; an HTTP provider that
// splits on commas mailed both). Lua parity. The C1 controls (U+0080-009F)
// are refused with the C0 ones: \s does not match U+0085 (NEL), a line
// break to some mail software, which the Lua twin already refused (audit 26).
const ADDR_RE = /^[^\s\x00-\x1f\x7f-\x9f@,;<>"()]+@[^\s\x00-\x1f\x7f-\x9f@,;<>"()]+\.[^\s\x00-\x1f\x7f-\x9f@,;<>"()]+$/;

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
    if (!addrOk(opts.from))
        emailError("invalid_argument", "invalid from address");
    if (!addrOk(opts.to))
        emailError("invalid_argument", "invalid to address");
    ccList(opts.cc);   // validated up front, for every provider
    // Reply-To goes into a header (SMTP) or a provider's JSON like to/from, so
    // it is held to the same address rule: no CR/LF, comma or second address.
    if (opts.reply_to !== undefined && opts.reply_to !== null && !addrOk(opts.reply_to))
        emailError("invalid_argument", "invalid reply_to address");

    const provider = opts.provider || "smtp";
    const fn = (typeof provider === "string"
                && Object.prototype.hasOwnProperty.call(providers, provider))
        ? providers[provider] : null;
    if (!fn)
        emailError("unknown_provider", "unknown provider: " + String(provider));
    return await fn(opts);
};

export { email };
