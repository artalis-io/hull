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
-- **Errors.** `email.send` follows the stdlib error convention
-- (docs/stdlib_style.md section 1): a failure to send THROWS. The thrown value
-- is a table carrying a stable `code` and a human `message` (with a `__tostring`
-- so an uncaught throw still prints cleanly). Success returns `true`. Callers
-- that want to branch use `pcall`:
--
--     local ok, err = pcall(email.send, opts)
--     if not ok then log.error(tostring(err))   -- err.code is stable
--         if err.code == "delivery_failed" then ... end
--     end
--
-- Codes: `invalid_argument` (missing/invalid opts, from, to, subject, body, or a
-- provider's required api_key), `unknown_provider`, `delivery_failed` (the
-- provider attempted delivery and it failed: transport error, no response, or a
-- non-2xx status). This composes with `hull/jobs`: a `send_email` handler that
-- just calls `email.send(job.data)` retries automatically on a thrown failure.
--
-- @module hull.email
-- @license AGPL-3.0-or-later
-- @usage
--   local email = require("hull.email")
--   email.send({
--       provider = "smtp",
--       from = "app@example.com",  to = "user@example.com",
--       subject = "Hello",         body = "Message body",
--       smtp_host = "smtp.example.com", smtp_port = 587,
--       smtp_user = "apikey",      smtp_pass = env.get("SMTP_PASS"),
--       smtp_tls  = true,
--   })   -- throws on failure; returns true on success

local json = require("hull.json")
local http_client = require("hull.http-client")
local smtp = require("hull.smtp")

local email = {}

-- Throw a coded error table. __tostring keeps an uncaught throw readable;
-- err.code is the stable, branch-on identity (docs/stdlib_style.md section 1).
local _err_mt = { __tostring = function(e) return e.message end }
local function email_error(code, message)
    error(setmetatable({ code = code, message = message }, _err_mt), 0)
end

-- Provider adapters: each takes (opts), returns true on success, throws on
-- failure (delivery_failed / invalid_argument).
local providers = {}

function providers.smtp(opts)
    local r = smtp.send({
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
    -- smtp.send is a thin C-cap binding that keeps its own {ok,error} shape;
    -- translate a failure into the email.send throw convention here.
    if not (r and r.ok) then
        email_error("delivery_failed", "smtp: " .. ((r and r.error) or "send failed"))
    end
    return true
end

function providers.postmark(opts)
    if not opts.api_key then
        email_error("invalid_argument", "postmark: api_key required")
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
        email_error("delivery_failed", "postmark: " .. tostring(resp))
    end
    if not resp then
        email_error("delivery_failed", "postmark: no response")
    end
    if resp.status >= 200 and resp.status < 300 then
        return true
    end
    email_error("delivery_failed", "postmark: " .. (resp.body or "unknown error"))
end

function providers.sendgrid(opts)
    if not opts.api_key then
        email_error("invalid_argument", "sendgrid: api_key required")
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
        email_error("delivery_failed", "sendgrid: " .. tostring(resp))
    end
    if not resp then
        email_error("delivery_failed", "sendgrid: no response")
    end
    if resp.status >= 200 and resp.status < 300 then
        return true
    end
    email_error("delivery_failed", "sendgrid: " .. (resp.body or "unknown error"))
end

function providers.resend(opts)
    if not opts.api_key then
        email_error("invalid_argument", "resend: api_key required")
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
        email_error("delivery_failed", "resend: " .. tostring(resp))
    end
    if not resp then
        email_error("delivery_failed", "resend: no response")
    end
    if resp.status >= 200 and resp.status < 300 then
        return true
    end
    email_error("delivery_failed", "resend: " .. (resp.body or "unknown error"))
end

--- Send an email via the selected provider.
--
-- Validates `from`/`to` against a simple `local@domain.tld` pattern before
-- dispatch. THROWS a coded error table (`.code`, `.message`) on any failure;
-- returns `true` on success. See the module header for the code list.
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
-- @treturn boolean  `true` on success. Throws `{code, message}` on failure.
function email.send(opts)
    if not opts then email_error("invalid_argument", "opts required") end
    if not opts.from then email_error("invalid_argument", "from required") end
    if not opts.to then email_error("invalid_argument", "to required") end
    if not opts.subject then email_error("invalid_argument", "subject required") end
    if not opts.body then email_error("invalid_argument", "body required") end

    -- Basic email format validation
    if not opts.from:match("^[^%s@]+@[^%s@]+%.[^%s@]+$") then
        email_error("invalid_argument", "invalid from address")
    end
    if not opts.to:match("^[^%s@]+@[^%s@]+%.[^%s@]+$") then
        email_error("invalid_argument", "invalid to address")
    end

    local provider = opts.provider or "smtp"
    local fn = providers[provider]
    if not fn then
        email_error("unknown_provider", "unknown provider: " .. tostring(provider))
    end
    return fn(opts)
end

return email
