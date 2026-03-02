// Todo App — full auth, CSRF, rate limiting, server-side rendered
//
// A todo list with user authentication, sessions, CSRF protection,
// and per-user data isolation. Pure HTML forms, no client-side JS.
//
// Run:  hull dev examples/todo/app.js -d /tmp/todo.db

import { app } from "hull:app";
import { cookie } from "hull:cookie";
import { crypto } from "hull:crypto";
import { db } from "hull:db";
import { log } from "hull:log";
import { auth } from "hull:middleware:auth";
import { csrf } from "hull:middleware:csrf";
import { logger } from "hull:middleware:logger";
import { ratelimit } from "hull:middleware:ratelimit";
import { session } from "hull:middleware:session";
import { template } from "hull:template";
import { time } from "hull:time";

app.manifest({});  // sandbox: no fs, no env, no outbound HTTP; default CSP

// ── Database setup ──────────────────────────────────────────────────

session.init({ ttl: 3600 });

db.exec(
    "CREATE TABLE IF NOT EXISTS users (" +
    "  id INTEGER PRIMARY KEY AUTOINCREMENT," +
    "  email TEXT UNIQUE NOT NULL," +
    "  password_hash TEXT NOT NULL," +
    "  name TEXT NOT NULL," +
    "  created_at INTEGER NOT NULL" +
    ")"
);

db.exec(
    "CREATE TABLE IF NOT EXISTS todos (" +
    "  id INTEGER PRIMARY KEY AUTOINCREMENT," +
    "  user_id INTEGER NOT NULL REFERENCES users(id)," +
    "  title TEXT NOT NULL," +
    "  done INTEGER NOT NULL DEFAULT 0," +
    "  created_at INTEGER NOT NULL" +
    ")"
);

db.exec("CREATE INDEX IF NOT EXISTS idx_todos_user ON todos (user_id)");

// ── CSRF secret ─────────────────────────────────────────────────────

function toHex(buf) {
    const bytes = new Uint8Array(buf);
    let hex = "";
    for (let i = 0; i < bytes.length; i++)
        hex += bytes[i].toString(16).padStart(2, "0");
    return hex;
}
const csrfSecret = toHex(crypto.random(32));

// ── Middleware stack ─────────────────────────────────────────────────

app.use("*", "/*", logger.middleware({ skip: ["/health"] }));

// Optional session loading (custom — JS auth middleware doesn't have optional flag)
app.use("*", "/*", (req, _res) => {
    const header = req.headers.cookie;
    if (!header) return 0;
    const cookies = cookie.parse(header);
    const sessionId = cookies["hull.sid"];
    if (sessionId) {
        const data = session.load(sessionId);
        if (data) {
            if (!req.ctx) req.ctx = {};
            req.ctx.session = data;
            req.ctx.sessionId = sessionId;
        }
    }
    return 0;
});

app.use("POST", "/login", ratelimit.middleware({ limit: 10, window: 60 }));
app.use("POST", "/register", ratelimit.middleware({ limit: 5, window: 60 }));
// CSRF needs body access → post-body middleware
app.usePost("*", "/*", csrf.middleware({ secret: csrfSecret }));

// ── Helpers ─────────────────────────────────────────────────────────

function parseForm(body) {
    const params = {};
    if (!body) return params;
    const pairs = body.split("&");
    for (let i = 0; i < pairs.length; i++) {
        const eqIdx = pairs[i].indexOf("=");
        if (eqIdx >= 0) {
            const k = decodeURIComponent(pairs[i].substring(0, eqIdx).replace(/\+/g, " "));
            const v = decodeURIComponent(pairs[i].substring(eqIdx + 1).replace(/\+/g, " "));
            params[k] = v;
        }
    }
    return params;
}

function requireSession(req, res) {
    if (!req.ctx || !req.ctx.session) {
        res.redirect("/login");
        return null;
    }
    return req.ctx.session;
}

function render(page, req, extra) {
    const ctx = Object.assign({}, extra || {});
    ctx.year = new Date().getFullYear().toString();
    ctx.csrf_token = req.ctx?.csrf_token ?? "";
    ctx.user = req.ctx?.session ?? null;
    ctx.logged_in = !!req.ctx?.session;
    return template.render(page, ctx);
}

// ── Health check ────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// ── Auth routes ─────────────────────────────────────────────────────

app.get("/login", (req, res) => {
    if (req.ctx?.session) {
        return res.redirect("/");
    }
    res.html(render("pages/login.html", req, { error: null }));
});

app.get("/register", (req, res) => {
    if (req.ctx?.session) {
        return res.redirect("/");
    }
    res.html(render("pages/register.html", req, { error: null }));
});

app.post("/login", (req, res) => {
    const form = parseForm(req.body);
    const email = form.email;
    const password = form.password;

    if (!email || !password) {
        return res.html(render("pages/login.html", req, { error: "Email and password are required" }));
    }

    const rows = db.query("SELECT * FROM users WHERE email = ?", [email]);
    if (rows.length === 0) {
        return res.html(render("pages/login.html", req, { error: "Invalid credentials" }));
    }

    const user = rows[0];
    if (!crypto.verifyPassword(password, user.password_hash)) {
        return res.html(render("pages/login.html", req, { error: "Invalid credentials" }));
    }

    auth.login(req, res, { user_id: user.id, email: user.email, name: user.name });
    res.redirect("/");
});

app.post("/register", (req, res) => {
    const form = parseForm(req.body);
    const email = form.email;
    const password = form.password;
    const name = form.name;

    if (!email) {
        return res.html(render("pages/register.html", req, { error: "Email is required" }));
    }
    if (!password || password.length < 8) {
        return res.html(render("pages/register.html", req, { error: "Password must be at least 8 characters" }));
    }
    if (!name) {
        return res.html(render("pages/register.html", req, { error: "Name is required" }));
    }

    const existing = db.query("SELECT id FROM users WHERE email = ?", [email]);
    if (existing.length > 0) {
        return res.html(render("pages/register.html", req, { error: "Email already registered" }));
    }

    const hash = crypto.hashPassword(password);
    db.exec("INSERT INTO users (email, password_hash, name, created_at) VALUES (?, ?, ?, ?)",
            [email, hash, name, time.now()]);
    const userId = db.lastId();

    auth.login(req, res, { user_id: userId, email: email, name: name });
    res.redirect("/");
});

app.post("/logout", (req, res) => {
    auth.logout(req, res);
    res.redirect("/login");
});

// ── Todo routes (authenticated) ─────────────────────────────────────

app.get("/", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const todos = db.query(
        "SELECT * FROM todos WHERE user_id = ? ORDER BY created_at DESC",
        [sess.user_id]);

    let doneCount = 0;
    for (let i = 0; i < todos.length; i++) {
        todos[i].done = (todos[i].done === 1);
        if (todos[i].done) doneCount++;
    }

    res.html(render("pages/index.html", req, {
        todos: todos,
        has_todos: todos.length > 0,
        total: todos.length,
        done_count: doneCount,
        remaining: todos.length - doneCount,
    }));
});

app.post("/add", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const form = parseForm(req.body);
    let title = form.title;
    if (!title || title.length === 0) {
        return res.redirect("/");
    }

    if (title.length > 500) title = title.substring(0, 500);

    db.exec("INSERT INTO todos (user_id, title, created_at) VALUES (?, ?, ?)",
            [sess.user_id, title, time.now()]);
    res.redirect("/");
});

app.post("/toggle/:id", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    db.exec(
        "UPDATE todos SET done = CASE WHEN done = 0 THEN 1 ELSE 0 END WHERE id = ? AND user_id = ?",
        [Number.parseInt(req.params.id), sess.user_id]);
    res.redirect("/");
});

app.post("/delete/:id", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    db.exec("DELETE FROM todos WHERE id = ? AND user_id = ?",
            [Number.parseInt(req.params.id), sess.user_id]);
    res.redirect("/");
});

log.info("Todo app loaded — routes registered");
