// Email — Hull + QuickJS example
//
// Run: SMTP_HOST=localhost SMTP_PORT=587 hull app.js -p 3000
// Contact-form / email-sending API with SQLite email log
//
// Environment variables:
//   SMTP_HOST  — SMTP server (default: localhost)
//   SMTP_PORT  — SMTP port (default: 587)
//   SMTP_USER  — username (optional, no auth if unset)
//   SMTP_PASS  — password (optional)
//   SMTP_FROM  — sender address (default: noreply@example.com)
//   SMTP_TLS   — "true" for STARTTLS, "false" for plain (default: true)

import { app } from "hull:app";
import { db } from "hull:db";
import { env } from "hull:env";
import { smtp } from "hull:smtp";
import { time } from "hull:time";
import { validate } from "hull:validate";

// Add your SMTP host to the hosts list for production use.
app.manifest({
    env: ["SMTP_HOST", "SMTP_PORT", "SMTP_USER", "SMTP_PASS", "SMTP_FROM", "SMTP_TLS"],
    hosts: ["localhost", "127.0.0.1", "smtp.gmail.com"],
});

// ── Config from env (lazy — env.get only works at request time) ──

function envGet(key, fallback) {
    try {
        const val = env.get(key);
        return val != null ? val : fallback;
    } catch (_e) { return fallback; }
}

let smtpCfg = null;

function getSmtpCfg() {
    if (smtpCfg) return smtpCfg;
    smtpCfg = {
        host: envGet("SMTP_HOST", "localhost"),
        port: parseInt(envGet("SMTP_PORT", "587"), 10),
        user: envGet("SMTP_USER", undefined),
        pass: envGet("SMTP_PASS", undefined),
        from: envGet("SMTP_FROM", "noreply@example.com"),
        tls:  envGet("SMTP_TLS", "true") !== "false",
    };
    return smtpCfg;
}

// ── Routes ────────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// Send an email
app.post("/send", (req, res) => {
    let body;
    try { body = JSON.parse(req.body); } catch (_e) {
        return res.status(400).json({ error: "invalid JSON" });
    }

    const [ok, errors] = validate.check(body, {
        to:      { required: true },
        subject: { required: true },
        body:    { required: true },
    });
    if (!ok) {
        return res.status(400).json({ errors });
    }

    const cfg = getSmtpCfg();
    const contentType = body.content_type || "text/plain";
    const cc = body.cc;             // array or undefined
    const replyTo = body.reply_to;

    // Build SMTP message
    const msg = {
        host:         cfg.host,
        port:         cfg.port,
        tls:          cfg.tls,
        from:         cfg.from,
        to:           body.to,
        subject:      body.subject,
        body:         body.body,
        content_type: contentType,
    };
    if (cfg.user) msg.username = cfg.user;
    if (cfg.pass) msg.password = cfg.pass;
    if (cc) msg.cc = cc;
    if (replyTo) msg.reply_to = replyTo;

    let result;
    try {
        result = smtp.send(msg);
    } catch (e) {
        result = { ok: false, error: String(e) };
    }

    // Log to database
    const ccStr = cc ? JSON.stringify(cc) : null;
    const statusStr = result.ok ? "sent" : "failed";
    const errorStr = result.error || null;

    db.exec(
        "INSERT INTO email_log (recipient, subject, body, content_type, cc, reply_to, status, error, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [body.to, body.subject, body.body, contentType, ccStr, replyTo, statusStr, errorStr, time.now()]
    );
    const id = db.lastId();

    if (result.ok) {
        res.json({ ok: true, id });
    } else {
        res.json({ ok: false, id, error: result.error });
    }
});

// List sent emails
app.get("/sent", (_req, res) => {
    const rows = db.query("SELECT id, recipient, subject, content_type, status, error, created_at FROM email_log ORDER BY id DESC LIMIT 50");
    res.json(rows);
});

// Get single email log entry
app.get("/sent/:id", (req, res) => {
    const rows = db.query("SELECT * FROM email_log WHERE id = ?", [req.params.id]);
    if (rows.length === 0) {
        return res.status(404).json({ error: "not found" });
    }
    res.json(rows[0]);
});
