// REST API — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// CRUD API for managing tasks with i18n error messages
//
// Graceful shutdown: Ctrl+C triggers drain mode (finishes in-flight
// requests within 5s). Second Ctrl+C stops immediately.

import { app } from "hull:app";
import { db } from "hull:db";
import { i18n } from "hull:i18n";
import { time } from "hull:time";
import { validate } from "hull:validate";

app.manifest({});

// ── i18n setup ─────────────────────────────────────────────────────

i18n.load("en", {
    error: {
        not_found:    "task not found",
        invalid_json: "invalid JSON",
    },
});

i18n.load("hu", {
    error: {
        not_found:    "feladat nem tal\u00e1lhat\u00f3",
        invalid_json: "\u00e9rv\u00e9nytelen JSON",
    },
});

i18n.locale("en");

function detectLang(req) {
    const lang = i18n.detect(req.headers["accept-language"]);
    if (lang) i18n.locale(lang);
}

// ── Routes ─────────────────────────────────────────────────────────

// List all tasks
app.get("/tasks", (req, res) => {
    detectLang(req);
    const tasks = db.query("SELECT * FROM tasks ORDER BY created_at DESC");
    res.json(tasks);
});

// Get a single task
app.get("/tasks/:id", (req, res) => {
    detectLang(req);
    const task = db.query("SELECT * FROM tasks WHERE id = ?", [req.params.id]);
    if (task.length === 0) {
        return res.status(404).json({ error: i18n.t("error.not_found") });
    }
    res.json(task[0]);
});

// Create a task
app.post("/tasks", (req, res) => {
    detectLang(req);
    let body;
    try { body = JSON.parse(req.body); } catch (_e) {
        return res.status(400).json({ error: i18n.t("error.invalid_json") });
    }
    const [ok, errors] = validate.check(body, {
        title: { required: true },
    });
    if (!ok) {
        return res.status(400).json({ errors });
    }
    db.exec("INSERT INTO tasks (title, created_at) VALUES (?, ?)",
            [body.title, time.now()]);
    const id = db.lastId();
    res.status(201).json({ id: id, title: body.title, done: 0 });
});

// Update a task
app.put("/tasks/:id", (req, res) => {
    detectLang(req);
    let body;
    try { body = JSON.parse(req.body); } catch (_e) {
        return res.status(400).json({ error: i18n.t("error.invalid_json") });
    }
    const changes = db.exec("UPDATE tasks SET title = ?, done = ? WHERE id = ?",
                            [body.title, body.done ? 1 : 0, req.params.id]);
    if (changes === 0) {
        return res.status(404).json({ error: i18n.t("error.not_found") });
    }
    res.json({ ok: true });
});

// Delete a task
app.del("/tasks/:id", (req, res) => {
    detectLang(req);
    const changes = db.exec("DELETE FROM tasks WHERE id = ?", [req.params.id]);
    if (changes === 0) {
        return res.status(404).json({ error: i18n.t("error.not_found") });
    }
    res.json({ ok: true });
});
