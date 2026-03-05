// JWT API — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Token-based auth API: register, login, protected routes using Bearer tokens

import { app } from "hull:app";
import { crypto } from "hull:crypto";
import { db } from "hull:db";
import { env } from "hull:env";
import { jwt } from "hull:jwt";
import { log } from "hull:log";
import { time } from "hull:time";
import { validate } from "hull:validate";

app.manifest({
    env: ["JWT_SECRET"],
});

// env.get() is unavailable at load time (env_cfg wired after manifest extraction),
// so fall back to a dev default.  Set JWT_SECRET in production.
let JWT_SECRET = "change-me-in-production";
try { const v = env.get("JWT_SECRET"); if (v) JWT_SECRET = v; } catch (_e) { /* env not ready */ }

// Middleware: extract and verify JWT on every request (optional — won't block)
app.use("*", "/*", (req, _res) => {
    const authHeader = req.headers.authorization;
    if (!authHeader) return 0;

    const match = authHeader.match(/^[Bb]earer\s+(.+)$/);
    if (!match) return 0;

    const result = jwt.verify(match[1], JWT_SECRET);
    // jwt.verify returns payload on success, [null, "reason"] on failure
    if (!Array.isArray(result)) {
        if (!req.ctx) req.ctx = {};
        req.ctx.user = result;
    }
    return 0;
});

// Helper: require authenticated user or respond 401
function requireAuth(req, res) {
    if (!req.ctx || !req.ctx.user) {
        res.status(401).json({ error: "authentication required" });
        return null;
    }
    return req.ctx.user;
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

// Login — returns JWT token
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

    // Issue JWT (expires in 1 hour)
    const token = jwt.sign({
        sub: user.id,
        email: user.email,
        exp: time.now() + 3600,
    }, JWT_SECRET);

    res.json({ token, user: { id: user.id, email: user.email, name: user.name } });
});

// Get current user (requires token)
app.get("/me", (req, res) => {
    const user = requireAuth(req, res);
    if (!user) return;

    const rows = db.query("SELECT id, email, name, created_at FROM users WHERE id = ?",
                          [user.sub]);
    if (rows.length === 0) {
        return res.status(404).json({ error: "user not found" });
    }

    res.json(rows[0]);
});

// Refresh token (requires valid token, returns new one)
app.post("/refresh", (req, res) => {
    const user = requireAuth(req, res);
    if (!user) return;

    const token = jwt.sign({
        sub: user.sub,
        email: user.email,
        exp: time.now() + 3600,
    }, JWT_SECRET);

    res.json({ token });
});

log.info("JWT API loaded — routes registered");
