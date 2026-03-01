// JWT API — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Token-based auth API: register, login, protected routes using Bearer tokens

import { app } from "hull:app";
import { crypto } from "hull:crypto";
import { db } from "hull:db";
import { jwt } from "hull:jwt";
import { log } from "hull:log";
import { time } from "hull:time";

const JWT_SECRET = "change-me-in-production";

// Initialize database
db.exec(
    "CREATE TABLE IF NOT EXISTS users (" +
    "  id INTEGER PRIMARY KEY AUTOINCREMENT," +
    "  email TEXT UNIQUE NOT NULL," +
    "  password_hash TEXT NOT NULL," +
    "  name TEXT NOT NULL," +
    "  created_at INTEGER" +
    ")"
);

// Middleware: extract and verify JWT on every request (optional — won't block)
app.use("*", "/*", (req, _res) => {
    const authHeader = req.headers.authorization;
    if (!authHeader) return 0;

    const match = authHeader.match(/^[Bb]earer\s+(.+)$/);
    if (!match) return 0;

    const result = jwt.verify(match[1], JWT_SECRET);
    // jwt.verify returns payload on success, [null, "reason"] on failure
    if (!Array.isArray(result)) {
        req.ctx = { user: result };
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
    const body = JSON.parse(req.body);
    if (!body) {
        return res.status(400).json({ error: "invalid JSON" });
    }

    const { email, password, name } = body;

    if (!email) {
        return res.status(400).json({ error: "email is required" });
    }
    if (!password || password.length < 8) {
        return res.status(400).json({ error: "password must be at least 8 characters" });
    }
    if (!name) {
        return res.status(400).json({ error: "name is required" });
    }

    const existing = db.query("SELECT id FROM users WHERE email = ?", [email]);
    if (existing.length > 0) {
        return res.status(409).json({ error: "email already registered" });
    }

    const hash = crypto.hashPassword(password);
    db.exec("INSERT INTO users (email, password_hash, name, created_at) VALUES (?, ?, ?, ?)",
            [email, hash, name, time.now()]);
    const id = db.lastId();

    res.status(201).json({ id, email, name });
});

// Login — returns JWT token
app.post("/login", (req, res) => {
    const body = JSON.parse(req.body);
    if (!body) {
        return res.status(400).json({ error: "invalid JSON" });
    }

    const { email, password } = body;
    if (!email || !password) {
        return res.status(400).json({ error: "email and password are required" });
    }

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
