// REST API — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// CRUD API for managing tasks

import { app } from "hull:app";
import { db } from "hull:db";
import { time } from "hull:time";
import { crypto } from "hull:crypto";

// Initialize database
db.exec(`CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    done INTEGER DEFAULT 0,
    created_at INTEGER
)`);

// List all tasks
app.get("/tasks", (req, res) => {
    const tasks = db.query("SELECT * FROM tasks ORDER BY created_at DESC");
    res.json(tasks);
});

// Get a single task
app.get("/tasks/:id", (req, res) => {
    const task = db.query("SELECT * FROM tasks WHERE id = ?", [req.params.id]);
    if (task.length === 0) {
        return res.status(404).json({ error: "task not found" });
    }
    res.json(task[0]);
});

// Create a task
app.post("/tasks", (req, res) => {
    const body = JSON.parse(req.body);
    if (!body.title) {
        return res.status(400).json({ error: "title is required" });
    }
    db.exec("INSERT INTO tasks (title, created_at) VALUES (?, ?)",
            [body.title, time.now()]);
    const id = db.lastId();
    res.status(201).json({ id: id, title: body.title, done: 0 });
});

// Update a task
app.put("/tasks/:id", (req, res) => {
    const body = JSON.parse(req.body);
    const changes = db.exec("UPDATE tasks SET title = ?, done = ? WHERE id = ?",
                            [body.title, body.done ? 1 : 0, req.params.id]);
    if (changes === 0) {
        return res.status(404).json({ error: "task not found" });
    }
    res.json({ ok: true });
});

// Delete a task
app.del("/tasks/:id", (req, res) => {
    const changes = db.exec("DELETE FROM tasks WHERE id = ?", [req.params.id]);
    if (changes === 0) {
        return res.status(404).json({ error: "task not found" });
    }
    res.json({ ok: true });
});
