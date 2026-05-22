--- Outbound email with pluggable provider dispatch.
--
-- Wraps four delivery backends behind one `email.send(opts)` call:
--
--   - `"smtp"`     — direct SMTP via the C `smtp.send` binding.
--   - `"postmark"` — Postmark HTTPS API.
--   - `"sendgrid"` — SendGrid HTTPS API.
--   - `"resend"`   — Resend HTTPS API.
--
-- API providers use `http_client.async.post` so they cooperate with the event
-- loop. SMTP delivery goes through the C `smtp.send()` cap which handles
-- TLS via mbedTLS and the embedded Mozilla CA bundle.
--
-- @module hull.email
-- @license AGPL-3.0-or-later
-- @usage
--   local email = require("hull.email")
--   local result = email.send({
--       provider = "smtp",
--       from = "app@example.com",  to = "user@example.com",
--       subject = "Hello",         body = "Message body",
--       smtp_host = "smtp.example.com", smtp_port = 587,
--       smtp_user = "apikey",      smtp_pass = env.get("SMTP_PASS"),
--       smtp_tls  = true,
--   })
--   if not result.ok then log.error(result.error) end

local log = require("hull.log")
local json = require("hull.json")
local http_client = require("hull.http-client")
local smtp = require("hull.smtp")

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

    local ok, resp = pcall(http_client.async.post,
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
    if not ok then
        return { ok = false, error = "postmark: " .. tostring(resp) }
    end
    if not resp then
        return { ok = false, error = "postmark: no response" }
    end
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

    local ok, resp = pcall(http_client.async.post,
        "https://api.sendgrid.com/v3/mail/send",
        json.encode(payload),
        {
            headers = {
                ["Authorization"] = "Bearer " .. opts.api_key,
                ["Content-Type"] = "application/json",
            },
        }
    )
    if not ok then
        return { ok = false, error = "sendgrid: " .. tostring(resp) }
    end
    if not resp then
        return { ok = false, error = "sendgrid: no response" }
    end
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

    local ok, resp = pcall(http_client.async.post,
        "https://api.resend.com/emails",
        json.encode(payload),
        {
            headers = {
                ["Authorization"] = "Bearer " .. opts.api_key,
                ["Content-Type"] = "application/json",
            },
        }
    )
    if not ok then
        return { ok = false, error = "resend: " .. tostring(resp) }
    end
    if not resp then
        return { ok = false, error = "resend: no response" }
    end
    if resp.status >= 200 and resp.status < 300 then
        return { ok = true }
    end
    return { ok = false, error = "resend: " .. (resp.body or "unknown error") }
end

--- Send an email via the selected provider.
--
-- Always validates `from`/`to` against a simple `local@domain.tld`
-- pattern before dispatch; provider-specific errors are surfaced as
-- `{ok=false, error="<provider>: <msg>"}`.
--
-- @function email.send
-- @tparam table opts
-- @tparam[opt="smtp"] string opts.provider  `"smtp"`, `"postmark"`,
--   `"sendgrid"`, or `"resend"`.
-- @tparam string opts.from     Sender address.
-- @tparam string opts.to       Recipient address.
-- @tparam string opts.subject  Subject line.
-- @tparam string opts.body     Body bytes.
-- @tparam[opt] {string,...} opts.cc           Cc recipients.
-- @tparam[opt] string opts.reply_to           Reply-To address.
-- @tparam[opt="text/plain"] string opts.content_type
--   `"text/plain"` or `"text/html"`.
-- @tparam[opt] string opts.api_key            Required for API providers.
-- @tparam[opt] string opts.smtp_host          SMTP server host.
-- @tparam[opt=587] number opts.smtp_port      SMTP server port.
-- @tparam[opt] string opts.smtp_user          SMTP username.
-- @tparam[opt] string opts.smtp_pass          SMTP password.
-- @tparam[opt=true] boolean opts.smtp_tls     Enable STARTTLS.
-- @treturn table  `{ok = true}` on success, `{ok = false, error = "..."}` on failure.
function email.send(opts)
    if not opts then return { ok = false, error = "opts required" } end
    if not opts.from then return { ok = false, error = "from required" } end
    if not opts.to then return { ok = false, error = "to required" } end
    if not opts.subject then return { ok = false, error = "subject required" } end
    if not opts.body then return { ok = false, error = "body required" } end

    -- Basic email format validation
    if not opts.from:match("^[^%s@]+@[^%s@]+%.[^%s@]+$") then
        return { ok = false, error = "invalid from address" }
    end
    if not opts.to:match("^[^%s@]+@[^%s@]+%.[^%s@]+$") then
        return { ok = false, error = "invalid to address" }
    end

    local provider = opts.provider or "smtp"
    local fn = providers[provider]
    if not fn then
        return { ok = false, error = "unknown provider: " .. tostring(provider) }
    end
    return fn(opts)
end

return email
