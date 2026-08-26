// Auth Example - Hull + QuickJS
//
// Run: hull app.js -p 3000
// Session-based auth API: register, login, logout, protected routes

import { app } from "hull:app";
import { crypto } from "hull:crypto";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { log } from "hull:log";
import { time } from "hull:time";
import { validate } from "hull:validate";
import { cookie } from "hull:web:cookie";
import { auth } from "hull:web:middleware:auth";
import { session } from "hull:web:middleware:session";

app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/log@1",
        "hull/web/cookie@1",
        "hull/crypto@1",
        "hull/db@1",
        "hull/time@1",
        "hull/validate@1",
        "hull/web/middleware/auth@1",
        "hull/web/middleware/session@1",
    ],
});

// Initialize sessions
session.init({ ttl: 3600 });

// Load session on every request (optional - won't block unauthenticated).
// The JS sessionMiddleware doesn't have an "optional" flag, so we use a
// lightweight custom middleware that attaches the session when present.
app.use("*", "/*", (req, _res) => {
    const header = req.headers.cookie;
    if (!header) return 0;

    const cookies = cookie.parse(header);
    const sessionId = cookies.hull_session;
    if (sessionId) {
        const data = session.load(sessionId);
        if (data) {
            if (!req.ctx) req.ctx = {};
            req.ctx.session_id = sessionId;
            req.ctx.session = data;
        }
    }
    return 0;
});

// Helper: require session or respond 401
function requireSession(req, res) {
    if (!req.ctx || !req.ctx.session) {
        res.status(401).json({ error: "authentication required" });
        return null;
    }
    return req.ctx.session;
}

// Health check
app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// Register
app.post("/register", (req, res) => {
    let body;
    try { body = JSON.parse(req.body); } catch (_e) { body = null; }
    if (!body) {
        return res.status(400).json({ error: "invalid JSON" });
    }

    const [ok, errors] = validate.check(body, {
        email:    { required: true },
        password: { required: true, min: 8 },
        name:     { required: true },
    });
    if (!ok) {
        return res.status(400).json({ errors });
    }

    const { email, password, name } = body;

    // Atomic check+insert to prevent TOCTOU race on email uniqueness
    const hash = crypto.hashPassword(password);
    let id;
    try {
        db.batch(() => {
            const existing = db.query("SELECT id FROM users WHERE email = ?", [email]);
            if (existing.length > 0) {
                throw new Error("email already registered");
            }
            db.exec("INSERT INTO users (email, password_hash, name, created_at) VALUES (?, ?, ?, ?)",
                    [email, hash, name, time.now()]);
            id = db.lastId();
        });
    } catch (e) {
        if (String(e).includes("email already registered")) {
            return res.status(409).json({ error: "email already registered" });
        }
        return res.status(500).json({ error: "registration failed" });
    }

    res.status(201).json({ id, email, name });
});

// Login
app.post("/login", (req, res) => {
    let body;
    try { body = JSON.parse(req.body); } catch (_e) { body = null; }
    if (!body) {
        return res.status(400).json({ error: "invalid JSON" });
    }

    const [ok, errors] = validate.check(body, {
        email:    { required: true },
        password: { required: true },
    });
    if (!ok) {
        return res.status(400).json({ errors });
    }

    const { email, password } = body;

    const rows = db.query("SELECT * FROM users WHERE email = ?", [email]);
    if (rows.length === 0) {
        return res.status(401).json({ error: "invalid credentials" });
    }

    const user = rows[0];
    if (!crypto.verifyPassword(password, user.password_hash)) {
        return res.status(401).json({ error: "invalid credentials" });
    }

    // secure: false lets `hull dev` (plain HTTP on :8080) work in a
    // real browser. Production over HTTPS should remove this opt
    // (defaults to true) so the cookie is only sent over TLS.
    auth.login(req, res, { user_id: user.id, email: user.email },
               { cookieOpts: { secure: false } });
    res.json({ id: user.id, email: user.email, name: user.name });
});

// Logout (requires session)
app.post("/logout", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    auth.logout(req, res);
    res.json({ ok: true });
});

// Get current user (requires session)
app.get("/me", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const rows = db.query("SELECT id, email, name, created_at FROM users WHERE id = ?",
                          [sess.user_id]);
    if (rows.length === 0) {
        return res.status(404).json({ error: "user not found" });
    }

    res.json(rows[0]);
});

log.info("Auth app loaded - routes registered");
