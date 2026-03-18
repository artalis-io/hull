<!-- minimal -->
## Database

SQLite database accessed via the `db` global. All queries use parameterized binding (SQL injection is impossible).

```lua
-- Lua
local rows = db.query("SELECT * FROM users WHERE id = ?", 1)
-- rows = {{ id = 1, name = "Alice" }}

db.exec("INSERT INTO users (name, email) VALUES (?, ?)", "Alice", "alice@example.com")

db.batch(function()
    db.exec("UPDATE accounts SET balance = balance - ? WHERE id = ?", 100, 1)
    db.exec("UPDATE accounts SET balance = balance + ? WHERE id = ?", 100, 2)
end)
```

```javascript
// JS
const rows = db.query("SELECT * FROM users WHERE id = ?", 1);
db.exec("INSERT INTO users (name, email) VALUES (?, ?)", "Alice", "alice@example.com");
db.batch(() => {
    db.exec("UPDATE accounts SET balance = balance - ? WHERE id = ?", 100, 1);
    db.exec("UPDATE accounts SET balance = balance + ? WHERE id = ?", 100, 2);
});
```

<!-- compact -->
## API

- **`db.query(sql, ...params)`** — returns array of row objects. Empty array if no results.
- **`db.exec(sql, ...params)`** — executes statement, returns nothing. Use for INSERT/UPDATE/DELETE/CREATE.
- **`db.batch(fn)`** — wraps `fn` in `BEGIN IMMEDIATE` ... `COMMIT`. Rolls back on error.

## Parameter Binding

Parameters are positional `?` placeholders. Supported types: string, number, boolean, nil/null, blob.

```lua
db.query("SELECT * FROM items WHERE price > ? AND category = ?", 9.99, "books")
db.exec("INSERT INTO logs (data) VALUES (?)", nil)  -- inserts NULL
```

## Protected Namespace

Tables prefixed with `_hull_` are reserved for Hull internals. User code cannot read or write them:
- `_hull_sessions`, `_hull_outbox`, `_hull_inbox_processed`, `_hull_idempotency_keys`, `_hull_migrations`

Stdlib modules (session, outbox, etc.) bypass this check automatically.

## Migrations

Place SQL files in `migrations/` numbered `001_create_users.sql`, `002_add_index.sql`, etc.

```sql
-- migrations/001_create_users.sql
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    email TEXT UNIQUE NOT NULL
);
```

Migrations run automatically on `hull dev` startup and during `hull test`. Each runs in its own transaction. Use `hull migrate status` to check state.

<!-- full -->
## Transaction Middleware

For request-scoped transactions, use the transaction middleware:

```lua
local transaction = require("hull.middleware.transaction")
app.use_post("POST", "/api/*", transaction.middleware())

app.post("/api/transfer", function(req, res)
    transaction.run(function()
        db.exec("UPDATE accounts SET balance = balance - ? WHERE id = ?", req.body.amount, req.body.from)
        db.exec("UPDATE accounts SET balance = balance + ? WHERE id = ?", req.body.amount, req.body.to)
    end)
    res.json({ ok = true })
end)
```

`transaction.try(fn)` returns `(ok, err)` instead of throwing on error.

## Common Patterns

```lua
-- Check existence
local rows = db.query("SELECT 1 FROM users WHERE email = ?", email)
if #rows > 0 then
    -- user exists
end

-- Get single row
local rows = db.query("SELECT * FROM users WHERE id = ?", id)
local user = rows[1]  -- nil if not found

-- Count
local rows = db.query("SELECT COUNT(*) as count FROM users")
local count = rows[1].count

-- Pagination
local rows = db.query("SELECT * FROM items ORDER BY id LIMIT ? OFFSET ?", page_size, (page - 1) * page_size)
```

## Migration Commands

```bash
hull migrate new add_posts      # creates migrations/002_add_posts.sql
hull migrate                    # run pending migrations
hull migrate status             # show applied/pending
hull dev --no-migrate           # skip auto-migration on startup
```

## Edge Cases

- `db.query` always returns an array, even for single-row results. Access with `rows[1]`.
- `db.batch` uses `BEGIN IMMEDIATE` (write lock). Nested `db.batch` calls are not supported.
- NULL values in query results are represented as `nil` (Lua) / `null` (JS).
- SQLite integers up to 2^53 are safe in both runtimes. Beyond that, precision may be lost.
- Boolean parameters are bound as integers (0/1). Query results return integers, not booleans.
