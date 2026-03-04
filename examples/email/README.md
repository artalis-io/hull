# Email Example

Contact-form / email-sending API — accept email requests via HTTP, validate, send via SMTP, and log results to SQLite.

## Routes

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/health` | Health check |
| `POST` | `/send` | Send an email (JSON body) |
| `GET` | `/sent` | List sent email log |
| `GET` | `/sent/:id` | Get single email log entry |

## Quick Start

```bash
# Dev server with mock SMTP (no real email sent)
make mock           # starts mock SMTP in background, prints port
SMTP_HOST=127.0.0.1 SMTP_PORT=<mock_port> SMTP_TLS=false make dev

# Send a test email
curl -X POST http://localhost:3000/send \
  -H 'Content-Type: application/json' \
  -d '{"to":"user@example.com","subject":"Hello","body":"Test message"}'

# Check the log
curl http://localhost:3000/sent
```

## Gmail Setup

To send real email through Gmail:

1. **Enable 2FA** on your Google account at [myaccount.google.com/security](https://myaccount.google.com/security)
2. **Generate an App Password** at [myaccount.google.com/apppasswords](https://myaccount.google.com/apppasswords) — Google gives you a 16-character code (e.g. `abcd efgh ijkl mnop`)
3. **Run with Gmail SMTP:**

```bash
SMTP_HOST=smtp.gmail.com \
SMTP_PORT=587 \
SMTP_TLS=true \
SMTP_USER=you@gmail.com \
SMTP_PASS="abcdefghijklmnop" \
SMTP_FROM=you@gmail.com \
make dev
```

The app uses STARTTLS on port 587 with AUTH PLAIN — the standard Gmail SMTP flow. The App Password acts as a regular password at the SMTP level, bypassing 2FA.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `SMTP_HOST` | `localhost` | SMTP server hostname |
| `SMTP_PORT` | `587` | SMTP server port |
| `SMTP_USER` | *(none)* | Username for AUTH PLAIN (optional) |
| `SMTP_PASS` | *(none)* | Password for AUTH PLAIN (optional) |
| `SMTP_FROM` | `noreply@example.com` | Sender address |
| `SMTP_TLS` | `true` | `true` for STARTTLS, `false` for plain TCP |

The manifest declares an allowlist of SMTP hosts: `localhost`, `127.0.0.1`, and `smtp.gmail.com`. To use a different SMTP provider, add its hostname to the `hosts` array in `app.manifest()` — this is Hull's security sandbox preventing arbitrary outbound connections.

## `POST /send` Body

```json
{
  "to": "user@example.com",
  "subject": "Hello",
  "body": "Message body",
  "content_type": "text/html",
  "cc": ["admin@example.com"],
  "reply_to": "support@example.com"
}
```

Required fields: `to`, `subject`, `body`. Everything else is optional.

## Production Build

```bash
make prod               # native binary
make prod CC=cosmocc    # portable APE binary (Cosmopolitan C)
make run                # run the production binary
```

## Testing

```bash
make test               # unit tests (both Lua + JS)
```

Unit tests run without a real SMTP server — `smtp.send()` fails in test mode but the error is caught, logged to the database, and returned in the response.

## Mock SMTP Server

A standalone mock SMTP server (`mock_smtp.c`) is included for e2e testing. It binds to an ephemeral port, accepts one SMTP connection, captures the DATA payload, and exits.

```bash
# Compile and run manually
cc -o /tmp/mock_smtp mock_smtp.c
/tmp/mock_smtp /tmp/smtp_port /tmp/smtp_data &
cat /tmp/smtp_port   # discover the port

# After sending an email through it:
cat /tmp/smtp_data   # see the captured message
```

The e2e test suite (`tests/e2e_examples.sh`) uses this automatically.
