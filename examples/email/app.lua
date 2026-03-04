-- Email — Hull + Lua example
--
-- Run: SMTP_HOST=localhost SMTP_PORT=587 hull app.lua -p 3000
-- Contact-form / email-sending API with SQLite email log
--
-- Environment variables:
--   SMTP_HOST  — SMTP server (default: localhost)
--   SMTP_PORT  — SMTP port (default: 587)
--   SMTP_USER  — username (optional, no auth if unset)
--   SMTP_PASS  — password (optional)
--   SMTP_FROM  — sender address (default: noreply@example.com)
--   SMTP_TLS   — "true" for STARTTLS, "false" for plain (default: true)

local validate = require("hull.validate")

-- Add your SMTP host to the hosts list for production use.
app.manifest({
    env = {"SMTP_HOST", "SMTP_PORT", "SMTP_USER", "SMTP_PASS", "SMTP_FROM", "SMTP_TLS"},
    hosts = {"localhost", "127.0.0.1", "smtp.gmail.com"},
})

-- ── Config from env (lazy — env.get only works at request time) ──

local smtp_cfg

local function env_get(key, default)
    local ok, val = pcall(env.get, key)
    if ok and val then return val end
    return default
end

local function get_smtp_cfg()
    if smtp_cfg then return smtp_cfg end
    smtp_cfg = {
        host = env_get("SMTP_HOST", "localhost"),
        port = tonumber(env_get("SMTP_PORT", "587")),
        user = env_get("SMTP_USER", nil),
        pass = env_get("SMTP_PASS", nil),
        from = env_get("SMTP_FROM", "noreply@example.com"),
        tls  = env_get("SMTP_TLS", "true") ~= "false",
    }
    return smtp_cfg
end

-- ── Routes ────────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- Send an email
app.post("/send", function(req, res)
    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local ok, errors = validate.check(body, {
        to      = { required = true },
        subject = { required = true },
        body    = { required = true },
    })
    if not ok then
        return res:status(400):json({ errors = errors })
    end

    local cfg = get_smtp_cfg()
    local content_type = body.content_type or "text/plain"
    local cc = body.cc           -- array or nil
    local reply_to = body.reply_to

    -- Build SMTP message
    local msg = {
        host         = cfg.host,
        port         = cfg.port,
        tls          = cfg.tls,
        from         = cfg.from,
        to           = body.to,
        subject      = body.subject,
        body         = body.body,
        content_type = content_type,
    }
    if cfg.user then msg.username = cfg.user end
    if cfg.pass then msg.password = cfg.pass end
    if cc then msg.cc = cc end
    if reply_to then msg.reply_to = reply_to end

    local send_ok, result = pcall(smtp.send, msg)
    if not send_ok then
        result = { ok = false, error = tostring(result) }
    end

    -- Log to database
    local cc_str = cc and json.encode(cc) or nil
    local status_str = result.ok and "sent" or "failed"
    local error_str = result.error or nil

    db.exec(
        "INSERT INTO email_log (recipient, subject, body, content_type, cc, reply_to, status, error, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        { body.to, body.subject, body.body, content_type, cc_str, reply_to, status_str, error_str, time.now() }
    )
    local id = db.last_id()

    if result.ok then
        res:json({ ok = true, id = id })
    else
        res:json({ ok = false, id = id, error = result.error })
    end
end)

-- List sent emails
app.get("/sent", function(_req, res)
    local rows = db.query("SELECT id, recipient, subject, content_type, status, error, created_at FROM email_log ORDER BY id DESC LIMIT 50")
    res:json(rows)
end)

-- Get single email log entry
app.get("/sent/:id", function(req, res)
    local rows = db.query("SELECT * FROM email_log WHERE id = ?", { req.params.id })
    if #rows == 0 then
        return res:status(404):json({ error = "not found" })
    end
    res:json(rows[1])
end)
