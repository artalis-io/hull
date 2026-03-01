--
-- Todo App — pure HTML, server-side rendered
--
-- A minimal todo list using Hull templates, SQLite, and plain HTML forms.
-- No client-side JavaScript — every action is a form POST with redirect.
--
-- Run:  hull dev examples/todo/app.lua -d /tmp/todo.db
--

local template = require("hull.template")

-- ── Database setup ──────────────────────────────────────────────────

db.exec([[
    CREATE TABLE IF NOT EXISTS todos (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        title      TEXT    NOT NULL,
        done       INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
    )
]])

-- ── Helpers ─────────────────────────────────────────────────────────

-- Parse application/x-www-form-urlencoded body into a table
local function parse_form(body)
    local params = {}
    if not body then return params end
    for pair in body:gmatch("[^&]+") do
        local k, v = pair:match("^([^=]*)=(.*)$")
        if k then
            -- Decode percent-encoding and plus signs
            k = k:gsub("+", " "):gsub("%%(%x%x)", function(h)
                return string.char(tonumber(h, 16))
            end)
            v = v:gsub("+", " "):gsub("%%(%x%x)", function(h)
                return string.char(tonumber(h, 16))
            end)
            params[k] = v
        end
    end
    return params
end

-- ── Routes ──────────────────────────────────────────────────────────

app.get("/", function(req, res)
    local todos = db.query("SELECT * FROM todos ORDER BY created_at DESC")

    local done_count = 0
    for _, t in ipairs(todos) do
        t.done = (t.done == 1)
        if t.done then done_count = done_count + 1 end
    end

    local html = template.render("pages/index.html", {
        year       = time.date():sub(1, 4),
        todos      = todos,
        has_todos  = #todos > 0,
        total      = #todos,
        done_count = done_count,
        remaining  = #todos - done_count,
    })
    res:html(html)
end)

app.post("/add", function(req, res)
    local form = parse_form(req.body)
    local title = form.title
    if not title or #title == 0 then
        return res:redirect("/")
    end

    -- Cap title length
    if #title > 500 then title = title:sub(1, 500) end

    db.exec("INSERT INTO todos (title, created_at) VALUES (?, ?)",
            { title, time.now() })
    res:redirect("/")
end)

app.post("/toggle/:id", function(req, res)
    db.exec("UPDATE todos SET done = CASE WHEN done = 0 THEN 1 ELSE 0 END WHERE id = ?",
            { tonumber(req.params.id) })
    res:redirect("/")
end)

app.post("/delete/:id", function(req, res)
    db.exec("DELETE FROM todos WHERE id = ?",
            { tonumber(req.params.id) })
    res:redirect("/")
end)

app.get("/about", function(req, res)
    local html = template.render("pages/about.html", {
        year = time.date():sub(1, 4),
    })
    res:html(html)
end)
