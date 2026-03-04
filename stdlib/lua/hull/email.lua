-- hull.email — Outbound email with provider dispatch
--
-- Supports direct SMTP and API providers (Postmark, SendGrid, Resend).
-- SMTP goes through the C smtp.send() binding; API providers use http.post().
--
-- Usage:
--   local email = require("hull.email")
--   local result = email.send({
--       provider = "smtp",           -- or "postmark", "sendgrid", "resend"
--       from = "app@example.com",
--       to = "user@example.com",
--       subject = "Hello",
--       body = "Message body",
--       -- SMTP-specific:
--       smtp_host = "smtp.example.com",
--       smtp_port = 587,
--       smtp_user = "apikey",
--       smtp_pass = env.get("SMTP_PASS"),
--       smtp_tls = true,
--       -- API-specific:
--       api_key = env.get("POSTMARK_TOKEN"),
--       -- Optional:
--       cc = {"admin@example.com"},
--       reply_to = "support@example.com",
--       content_type = "text/html",
--   })
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local email = {}

-- Provider adapters: each takes (opts) and returns { ok, error }
local providers = {}

function providers.smtp(opts)
    return smtp.send({
        host = opts.smtp_host,
        port = opts.smtp_port or 587,
        username = opts.smtp_user,
        password = opts.smtp_pass,
        tls = opts.smtp_tls ~= false,
        from = opts.from,
        to = opts.to,
        cc = opts.cc,
        reply_to = opts.reply_to,
        subject = opts.subject,
        body = opts.body,
        content_type = opts.content_type or "text/plain",
    })
end

function providers.postmark(opts)
    if not opts.api_key then
        return { ok = false, error = "postmark: api_key required" }
    end

    local payload = {
        From = opts.from,
        To = opts.to,
        Subject = opts.subject,
    }
    if opts.cc then
        payload.Cc = table.concat(opts.cc, ",")
    end
    if opts.reply_to then
        payload.ReplyTo = opts.reply_to
    end
    if opts.content_type == "text/html" then
        payload.HtmlBody = opts.body
    else
        payload.TextBody = opts.body
    end

    local resp = http.post(
        "https://api.postmarkapp.com/email",
        json.encode(payload),
        {
            headers = {
                ["X-Postmark-Server-Token"] = opts.api_key,
                ["Content-Type"] = "application/json",
                ["Accept"] = "application/json",
            },
        }
    )
    if resp.status >= 200 and resp.status < 300 then
        return { ok = true }
    end
    return { ok = false, error = "postmark: " .. (resp.body or "unknown error") }
end

function providers.sendgrid(opts)
    if not opts.api_key then
        return { ok = false, error = "sendgrid: api_key required" }
    end

    local payload = {
        personalizations = {{ to = {{ email = opts.to }} }},
        from = { email = opts.from },
        subject = opts.subject,
        content = {{
            type = opts.content_type or "text/plain",
            value = opts.body,
        }},
    }
    if opts.reply_to then
        payload.reply_to = { email = opts.reply_to }
    end

    local resp = http.post(
        "https://api.sendgrid.com/v3/mail/send",
        json.encode(payload),
        {
            headers = {
                ["Authorization"] = "Bearer " .. opts.api_key,
                ["Content-Type"] = "application/json",
            },
        }
    )
    if resp.status >= 200 and resp.status < 300 then
        return { ok = true }
    end
    return { ok = false, error = "sendgrid: " .. (resp.body or "unknown error") }
end

function providers.resend(opts)
    if not opts.api_key then
        return { ok = false, error = "resend: api_key required" }
    end

    local payload = {
        from = opts.from,
        to = opts.to,
        subject = opts.subject,
    }
    if opts.content_type == "text/html" then
        payload.html = opts.body
    else
        payload.text = opts.body
    end
    if opts.reply_to then payload.reply_to = opts.reply_to end
    if opts.cc then payload.cc = opts.cc end

    local resp = http.post(
        "https://api.resend.com/emails",
        json.encode(payload),
        {
            headers = {
                ["Authorization"] = "Bearer " .. opts.api_key,
                ["Content-Type"] = "application/json",
            },
        }
    )
    if resp.status >= 200 and resp.status < 300 then
        return { ok = true }
    end
    return { ok = false, error = "resend: " .. (resp.body or "unknown error") }
end

--- Send an email.
-- @param opts table with fields:
--   provider: "smtp" | "postmark" | "sendgrid" | "resend" (default: "smtp")
--   from, to, subject, body: required
--   cc, reply_to, content_type: optional
--   api_key: required for API providers
--   smtp_host, smtp_port, smtp_user, smtp_pass, smtp_tls: required for SMTP
-- @return { ok = true } or { ok = false, error = "message" }
function email.send(opts)
    if not opts then return { ok = false, error = "opts required" } end
    if not opts.from then return { ok = false, error = "from required" } end
    if not opts.to then return { ok = false, error = "to required" } end
    if not opts.subject then return { ok = false, error = "subject required" } end
    if not opts.body then return { ok = false, error = "body required" } end

    local provider = opts.provider or "smtp"
    local fn = providers[provider]
    if not fn then
        return { ok = false, error = "unknown provider: " .. tostring(provider) }
    end
    return fn(opts)
end

return email
