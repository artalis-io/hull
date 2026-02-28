-- REST API — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- CRUD API for managing tasks

-- Initialize database
db.exec([[CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    done INTEGER DEFAULT 0,
    created_at INTEGER
)]])

-- List all tasks
app.get("/tasks", function(_req, res)
    local tasks = db.query("SELECT * FROM tasks ORDER BY created_at DESC")
    res:json(tasks)
end)

-- Get a single task
app.get("/tasks/:id", function(req, res)
    local task = db.query("SELECT * FROM tasks WHERE id = ?", { req.params.id })
    if #task == 0 then
        return res:status(404):json({ error = "task not found" })
    end
    res:json(task[1])
end)

-- Create a task
app.post("/tasks", function(req, res)
    local body = json.decode(req.body)
    if not body or not body.title then
        return res:status(400):json({ error = "title is required" })
    end
    db.exec("INSERT INTO tasks (title, created_at) VALUES (?, ?)",
            { body.title, time.now() })
    local id = db.last_id()
    res:status(201):json({ id = id, title = body.title, done = 0 })
end)

-- Update a task
app.put("/tasks/:id", function(req, res)
    local body = json.decode(req.body)
    local done = 0
    if body.done then done = 1 end
    local changes = db.exec("UPDATE tasks SET title = ?, done = ? WHERE id = ?",
                            { body.title, done, req.params.id })
    if changes == 0 then
        return res:status(404):json({ error = "task not found" })
    end
    res:json({ ok = true })
end)

-- Delete a task
app.del("/tasks/:id", function(req, res)
    local changes = db.exec("DELETE FROM tasks WHERE id = ?", { req.params.id })
    if changes == 0 then
        return res:status(404):json({ error = "task not found" })
    end
    res:json({ ok = true })
end)
