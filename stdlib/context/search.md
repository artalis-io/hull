<!-- minimal -->
## Full-Text Search

SQLite FTS5-backed full-text search.

```lua
-- Lua
local search = require("hull.search")

search.create_index("articles", { "title", "body" })
search.index("articles", 1, { title = "Hello World", body = "First post content" })
local results = search.query("articles", "hello")
-- results = {{ id = 1, rank = -0.5 }}
search.remove("articles", 1)
```

```javascript
// JS
import { search } from "hull:search";

search.createIndex("articles", ["title", "body"]);
search.index("articles", 1, { title: "Hello World", body: "First post content" });
const results = search.query("articles", "hello");
search.remove("articles", 1);
```

<!-- compact -->
## API

- **`search.create_index(name, columns, opts?)`** / `search.createIndex(...)` - create FTS5 virtual table
- **`search.index(name, id, fields)`** - insert or replace document
- **`search.remove(name, id)`** - delete document from index
- **`search.query(name, query, opts?)`** - full-text search, returns `[{ id, rank }]`
  - `opts.limit` - max results (default: 20)
  - `opts.offset` - skip results (default: 0)
- **`search.reindex(name, source_table, opts?)`** / `search.reindex(...)` - bulk re-index from existing table
- **`search.drop_index(name)`** / `search.dropIndex(...)` - drop FTS5 table

## FTS5 Query Syntax

FTS5 supports standard full-text query operators:

- `hello world` - match documents containing both terms
- `hello OR world` - match either term
- `"hello world"` - exact phrase match
- `hello*` - prefix match
- `NEAR(hello world, 5)` - terms within 5 tokens of each other
- `title:hello` - match in specific column only

## Keeping Index in Sync

Index updates must be done manually when the source data changes:

```lua
app.post("/api/articles", function(req, res)
    db.exec("INSERT INTO articles (title, body) VALUES (?, ?)", req.body.title, req.body.body)
    local rows = db.query("SELECT last_insert_rowid() as id")
    search.index("articles", rows[1].id, { title = req.body.title, body = req.body.body })
    res.status(201).json({ id = rows[1].id })
end)
```

<!-- full -->
## Complete Search Example

```lua
local search = require("hull.search")

-- Create index at startup
search.create_index("products", { "name", "description", "category" })

-- Bulk reindex from existing table
search.reindex("products", "products", {
    columns = { "name", "description", "category" }
})

app.get("/api/search", function(req, res)
    local q = req.query.q
    if not q or q == "" then
        return res.status(400).json({ error = "query required" })
    end
    local results = search.query("products", q, {
        limit = tonumber(req.query.limit) or 20,
        offset = tonumber(req.query.offset) or 0,
    })

    -- Fetch full records for matched IDs
    if #results == 0 then
        return res.json({ items = {}, total = 0 })
    end
    local ids = {}
    for i, r in ipairs(results) do
        ids[i] = r.id
    end
    local placeholders = string.rep("?,", #ids - 1) .. "?"
    local items = db.query(
        "SELECT * FROM products WHERE id IN (" .. placeholders .. ")",
        table.unpack(ids)
    )
    res.json({ items = items, total = #items })
end)
```

```javascript
import { search } from "hull:search";

search.createIndex("products", ["name", "description", "category"]);

app.get("/api/search", (req, res) => {
    const q = req.query.q;
    if (!q) return res.status(400).json({ error: "query required" });

    const results = search.query("products", q, {
        limit: Number(req.query.limit) || 20,
        offset: Number(req.query.offset) || 0,
    });

    if (results.length === 0) return res.json({ items: [], total: 0 });

    const placeholders = results.map(() => "?").join(",");
    const ids = results.map(r => r.id);
    const items = db.query(`SELECT * FROM products WHERE id IN (${placeholders})`, ...ids);
    res.json({ items, total: items.length });
});
```

## Notes

- FTS5 indexes are separate SQLite virtual tables. They don't auto-sync with source tables.
- `search.reindex` is useful for rebuilding the index after bulk data changes.
- Results are sorted by relevance (`rank` field, lower is more relevant).
- The FTS5 index is stored in the same SQLite database as your app data.
- For large datasets, consider paginating with `limit` and `offset`.
