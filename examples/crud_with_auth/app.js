// CRUD with Auth — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Tasks API with session-based auth — each user only sees their own tasks

import { app } from "hull:app";
import { auth } from "hull:auth";
import { cookie } from "hull:cookie";
import { crypto } from "hull:crypto";
import { db } from "hull:db";
import { log } from "hull:log";
import { session } from "hull:session";
import { time } from "hull:time";

// Initialize database
session.init({ ttl: 3600 });

db.exec(
    "CREATE TABLE IF NOT EXISTS users (" +
    "  id INTEGER PRIMARY KEY AUTOINCREMENT," +
    "  email TEXT UNIQUE NOT NULL," +
    "  password_hash TEXT NOT NULL," +
    "  name TEXT NOT NULL," +
    "  created_at INTEGER" +
    ")"
);

db.exec(
    "CREATE TABLE IF NOT EXISTS tasks (" +
    "  id INTEGER PRIMARY KEY AUTOINCREMENT," +
    "  user_id INTEGER NOT NULL," +
    "  title TEXT NOT NULL," +
    "  done INTEGER DEFAULT 0," +
    "  created_at INTEGER," +
    "  FOREIGN KEY (user_id) REFERENCES users(id)" +
    ")"
);

db.exec("CREATE INDEX IF NOT EXISTS idx_tasks_user ON tasks (user_id)");

// Load session on every request (optional — won't block unauthenticated)
app.use("*", "/*", (req, _res) => {
    const header = req.headers.cookie;
    if (!header) return 0;

    const cookies = cookie.parse(header);
    const sessionId = cookies["hull.sid"];
    if (sessionId) {
        const data = session.load(sessionId);
        if (data) {
            req.ctx = { sessionId, session: data };
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

// ── Auth routes ─────────────────────────────────────────────────────

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

    auth.login(req, res, { user_id: user.id, email: user.email });
    res.json({ id: user.id, email: user.email, name: user.name });
});

app.post("/logout", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    auth.logout(req, res);
    res.json({ ok: true });
});

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

// ── Task CRUD (scoped to current user) ──────────────────────────────

// List my tasks
app.get("/tasks", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const tasks = db.query("SELECT id, title, done, created_at FROM tasks WHERE user_id = ? ORDER BY created_at DESC",
                           [sess.user_id]);
    res.json(tasks);
});

// Get one of my tasks
app.get("/tasks/:id", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const rows = db.query("SELECT id, title, done, created_at FROM tasks WHERE id = ? AND user_id = ?",
                          [req.params.id, sess.user_id]);
    if (rows.length === 0) {
        return res.status(404).json({ error: "task not found" });
    }

    res.json(rows[0]);
});

// Create a task
app.post("/tasks", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const body = JSON.parse(req.body);
    if (!body || !body.title) {
        return res.status(400).json({ error: "title is required" });
    }

    db.exec("INSERT INTO tasks (user_id, title, created_at) VALUES (?, ?, ?)",
            [sess.user_id, body.title, time.now()]);
    const id = db.lastId();

    res.status(201).json({ id, title: body.title, done: 0 });
});

// Update a task
app.put("/tasks/:id", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const body = JSON.parse(req.body);
    if (!body) {
        return res.status(400).json({ error: "invalid JSON" });
    }

    const changes = db.exec("UPDATE tasks SET title = ?, done = ? WHERE id = ? AND user_id = ?",
                            [body.title, body.done ? 1 : 0, req.params.id, sess.user_id]);
    if (changes === 0) {
        return res.status(404).json({ error: "task not found" });
    }

    res.json({ ok: true });
});

// Delete a task
app.del("/tasks/:id", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const changes = db.exec("DELETE FROM tasks WHERE id = ? AND user_id = ?",
                            [req.params.id, sess.user_id]);
    if (changes === 0) {
        return res.status(404).json({ error: "task not found" });
    }

    res.json({ ok: true });
});

log.info("CRUD with Auth loaded — routes registered");
