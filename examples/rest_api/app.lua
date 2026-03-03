-- REST API — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- CRUD API for managing tasks with i18n error messages
--
-- Graceful shutdown: Ctrl+C triggers drain mode (finishes in-flight
-- requests within 5s). Second Ctrl+C stops immediately.

local validate = require("hull.validate")
local i18n     = require("hull.i18n")

app.manifest({})

-- ── i18n setup ─────────────────────────────────────────────────────

i18n.load("en", {
    error = {
        not_found    = "task not found",
        invalid_json = "invalid JSON",
    },
})

i18n.load("hu", {
    error = {
        not_found    = "feladat nem tal\xc3\xa1lhat\xc3\xb3",
        invalid_json = "\xc3\xa9rv\xc3\xa9nytelen JSON",
    },
})

i18n.locale("en")

-- Detect language from Accept-Language header per request
local function detect_lang(req)
    local lang = i18n.detect(req.headers["accept-language"])
    if lang then i18n.locale(lang) end
end

-- ── Routes ─────────────────────────────────────────────────────────

-- List all tasks
app.get("/tasks", function(req, res)
    detect_lang(req)
    local tasks = db.query("SELECT * FROM tasks ORDER BY created_at DESC")
    res:json(tasks)
end)

-- Get a single task
app.get("/tasks/:id", function(req, res)
    detect_lang(req)
    local task = db.query("SELECT * FROM tasks WHERE id = ?", { req.params.id })
    if #task == 0 then
        return res:status(404):json({ error = i18n.t("error.not_found") })
    end
    res:json(task[1])
end)

-- Create a task
app.post("/tasks", function(req, res)
    detect_lang(req)
    local body = json.decode(req.body)
    if not body then
        return res:status(400):json({ error = i18n.t("error.invalid_json") })
    end
    local ok, errors = validate.check(body, {
        title = { required = true },
    })
    if not ok then
        return res:status(400):json({ errors = errors })
    end
    db.exec("INSERT INTO tasks (title, created_at) VALUES (?, ?)",
            { body.title, time.now() })
    local id = db.last_id()
    res:status(201):json({ id = id, title = body.title, done = 0 })
end)

-- Update a task
app.put("/tasks/:id", function(req, res)
    detect_lang(req)
    local body = json.decode(req.body)
    if not body then
        return res:status(400):json({ error = i18n.t("error.invalid_json") })
    end
    local done = 0
    if body.done then done = 1 end
    local changes = db.exec("UPDATE tasks SET title = COALESCE(?, title), done = ? WHERE id = ?",
                            { body.title, done, req.params.id })
    if changes == 0 then
        return res:status(404):json({ error = i18n.t("error.not_found") })
    end
    res:json({ ok = true })
end)

-- Delete a task
app.del("/tasks/:id", function(req, res)
    detect_lang(req)
    local changes = db.exec("DELETE FROM tasks WHERE id = ?", { req.params.id })
    if changes == 0 then
        return res:status(404):json({ error = i18n.t("error.not_found") })
    end
    res:json({ ok = true })
end)
