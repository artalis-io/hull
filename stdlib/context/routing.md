<!-- minimal -->
## Routing

Register routes with `app.get()`, `app.post()`, `app.put()`, `app.delete()`.

```lua
-- Lua
app.get("/", function(req, res)
    res.json({ message = "hello" })
end)

app.get("/users/:id", function(req, res)
    local id = req.params.id
    res.json({ id = id })
end)

app.post("/users", function(req, res)
    local body = req.body
    db.exec("INSERT INTO users (name) VALUES (?)", body.name)
    res.status(201).json({ ok = true })
end)
```

```javascript
// JS
app.get("/", (req, res) => {
    res.json({ message: "hello" });
});

app.get("/users/:id", (req, res) => {
    res.json({ id: req.params.id });
});

app.post("/users", (req, res) => {
    db.exec("INSERT INTO users (name) VALUES (?)", req.body.name);
    res.status(201).json({ ok: true });
});
```

<!-- compact -->
## Request Object

- **`req.method`** — HTTP method (`"GET"`, `"POST"`, etc.)
- **`req.path`** — request path (`"/users/42"`)
- **`req.params`** — route parameters (`{ id = "42" }` for `/users/:id`)
- **`req.query`** — query string parameters (`{ page = "2" }` for `?page=2`)
- **`req.headers`** — request headers (lowercase keys)
- **`req.body`** — parsed request body (JSON object or string)
- **`req.ctx`** — mutable context table for middleware data passing

## Response Object

- **`res.json(data)`** — send JSON response with `Content-Type: application/json`
- **`res.text(str)`** — send plain text with `Content-Type: text/plain`
- **`res.html(str)`** — send HTML with `Content-Type: text/html`
- **`res.status(code)`** — set status code, chainable: `res.status(201).json(data)`
- **`res.header(name, value)`** — set response header, chainable
- **`res.redirect(url)`** — send 302 redirect
- **`res.redirect(url, code)`** — redirect with custom status (301, 303, etc.)

## Route Patterns

- `/users` — exact match
- `/users/:id` — named parameter (matches one segment)
- `/files/*` — wildcard (matches rest of path)

Parameters are always strings. Cast manually: `tonumber(req.params.id)` (Lua) / `Number(req.params.id)` (JS).

## Query String

```lua
-- GET /search?q=hello&page=2
app.get("/search", function(req, res)
    local q = req.query.q        -- "hello"
    local page = req.query.page  -- "2" (always string)
end)
```

<!-- full -->
## REST API Pattern

```lua
app.get("/api/items", function(req, res)
    local rows = db.query("SELECT * FROM items ORDER BY id")
    res.json(rows)
end)

app.get("/api/items/:id", function(req, res)
    local rows = db.query("SELECT * FROM items WHERE id = ?", tonumber(req.params.id))
    if #rows == 0 then
        return res.status(404).json({ error = "Not found" })
    end
    res.json(rows[1])
end)

app.post("/api/items", function(req, res)
    local body = req.body
    db.exec("INSERT INTO items (name, price) VALUES (?, ?)", body.name, body.price)
    res.status(201).json({ ok = true })
end)

app.put("/api/items/:id", function(req, res)
    local body = req.body
    db.exec("UPDATE items SET name = ?, price = ? WHERE id = ?",
        body.name, body.price, tonumber(req.params.id))
    res.json({ ok = true })
end)

app.delete("/api/items/:id", function(req, res)
    db.exec("DELETE FROM items WHERE id = ?", tonumber(req.params.id))
    res.json({ ok = true })
end)
```

```javascript
// JS equivalent
app.get("/api/items", (req, res) => {
    res.json(db.query("SELECT * FROM items ORDER BY id"));
});

app.get("/api/items/:id", (req, res) => {
    const rows = db.query("SELECT * FROM items WHERE id = ?", Number(req.params.id));
    if (rows.length === 0) return res.status(404).json({ error: "Not found" });
    res.json(rows[0]);
});

app.post("/api/items", (req, res) => {
    db.exec("INSERT INTO items (name, price) VALUES (?, ?)", req.body.name, req.body.price);
    res.status(201).json({ ok: true });
});
```

## Response Headers

```lua
app.get("/api/data", function(req, res)
    res.header("X-Custom", "value")
       .header("Cache-Control", "no-cache")
       .json({ data = "hello" })
end)
```

## Static Files

Place files in `app_dir/static/`. They are served automatically at `/static/*` with correct MIME types and ETag/304 support. User routes always take priority over static files.

## Important Notes

- Route handlers are registered in order. First match wins.
- Parameters are always strings -- never numbers.
- `req.body` is `nil` for GET/DELETE requests (no body).
- JSON request bodies are auto-parsed when `Content-Type: application/json`.
- For URL-encoded form bodies, use `form.parse(req.body)`.
