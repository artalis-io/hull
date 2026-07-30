--- Full-text search backed by SQLite FTS5 virtual tables.
--
-- Each search index is a `_hull_fts_<name>` FTS5 table with a fixed `id`
-- column plus the columns the caller declared. Identifiers are
-- validated against `[A-Za-z_][A-Za-z0-9_]*` and a small SQL-keyword
-- deny-list to prevent injection through `name` / column arguments.
--
-- Supports snippet/highlight extraction, ranked (`rank`) or insertion
-- order (`rowid`), and bulk re-indexing from a source table.
--
-- @module hull.search
-- @license AGPL-3.0-or-later
-- @usage
--   local search = require("hull.search")
--   search.create_index("posts", { "title", "body" })
--   search.index("posts", post_id, { title = post.title, body = post.body })
--   for _, hit in ipairs(search.query("posts", "hull AND wasm", { limit = 10 })) do
--       print(hit.id, hit.rank)
--   end

local db = require("hull.db").default()

local search = {}

-- FTS5 is a SQLite-only feature; there is no Postgres equivalent wired
-- here. Fail with a clear message rather than a cryptic "near VIRTUAL"
-- (or information_schema) SQL error when the default connection is not
-- SQLite. Same policy as db.udf.
local function require_sqlite()
    if db.backend_name ~= "sqlite" then
        error("hull/search requires the SQLite backend (FTS5); the current "
              .. "database connection is '" .. tostring(db.backend_name)
              .. "'. Full-text search is not available on this backend.")
    end
end

-- ── Helpers ───────────────────────────────────────────────────────────

local FTS_PREFIX = "_hull_fts_"

--- Validate an SQL identifier (table name, column name).
-- Must match [a-zA-Z_][a-zA-Z0-9_]* and must not start with _hull_.
local SQL_KEYWORDS = {
    SELECT=1, INSERT=1, UPDATE=1, DELETE=1, DROP=1, CREATE=1, ALTER=1,
    TABLE=1, INDEX=1, WHERE=1, FROM=1, INTO=1, VALUES=1, SET=1,
    UNION=1, JOIN=1, ORDER=1, GROUP=1, HAVING=1, LIMIT=1, OFFSET=1,
    BEGIN=1, COMMIT=1, ROLLBACK=1, PRAGMA=1, ATTACH=1, DETACH=1,
}

local function validate_identifier(name, label)
    if type(name) ~= "string" or name == "" then
        error("search: " .. label .. " must be a non-empty string")
    end
    if not string.match(name, "^[a-zA-Z_][a-zA-Z0-9_]*$") then
        error("search: invalid " .. label .. " '" .. name ..
              "' (must match [a-zA-Z_][a-zA-Z0-9_]*)")
    end
    if name:sub(1, 6) == "_hull_" then
        error("search: " .. label .. " must not start with '_hull_'")
    end
    if SQL_KEYWORDS[name:upper()] then
        error("search: " .. label .. " must not be a SQL keyword")
    end
end

--- Validate a list of column names. Returns the validated list.
local function validate_columns(columns)
    if type(columns) ~= "table" or #columns == 0 then
        error("search: columns must be a non-empty array of strings")
    end
    for i, col in ipairs(columns) do
        validate_identifier(col, "column[" .. i .. "]")
    end
    return columns
end

--- Build the FTS5 table name from a user-facing index name.
local function fts_table(name)
    return FTS_PREFIX .. name
end

-- ── Public API ────────────────────────────────────────────────────────

--- Create an FTS5 virtual table for full-text search. Idempotent.
--
-- @function search.create_index
-- @tparam string name         Index name (will become `_hull_fts_<name>`).
-- @tparam {string,...} columns  Searchable column names (≥ 1).
-- @tparam[opt] table opts
-- @tparam[opt="unicode61"] string opts.tokenize  FTS5 tokenizer spec.
-- @tparam[opt] string opts.content        External content table name.
-- @tparam[opt] string opts.content_rowid  Rowid column for external content.
-- @raise If `name`/columns fail identifier validation.
function search.create_index(name, columns, opts)
    require_sqlite()
    validate_identifier(name, "index name")
    validate_columns(columns)
    opts = opts or {}

    local tbl = fts_table(name)
    local col_list = "id, " .. table.concat(columns, ", ")

    -- Build FTS5 options
    local fts_opts = {}
    fts_opts[#fts_opts + 1] = col_list

    -- FTS5 tokenize names are short identifiers ("unicode61"), optionally
    -- followed by space-separated identifier-or-integer args
    -- ("porter ascii", "unicode61 remove_diacritics 1"). The shape is a
    -- sequence of [A-Za-z][A-Za-z0-9_]* (identifier) OR [0-9]+ (positive
    -- integer arg) separated by single spaces; the first token must be
    -- an identifier. Lua patterns can't express repeated groups so we
    -- split-and-validate manually. Mirror semantics in the JS sibling.
    local tokenize = opts.tokenize or "unicode61"
    local function valid_tokenize(s)
        if #s == 0 then return false end
        local idx = 0
        for part in (s .. " "):gmatch("([^ ]*) ") do
            idx = idx + 1
            if part == "" then return false end  -- empty → leading/trailing/double space
            if idx == 1 then
                if not part:match("^[a-zA-Z][a-zA-Z0-9_]*$") then return false end
            else
                if not (part:match("^[a-zA-Z][a-zA-Z0-9_]*$") or
                        part:match("^[0-9]+$")) then
                    return false
                end
            end
        end
        return idx > 0
    end
    if not valid_tokenize(tokenize) then
        error("search: invalid tokenize option " ..
              "(must match identifier [A-Za-z][A-Za-z0-9_]* with " ..
              "single-space-separated identifier-or-integer args)")
    end
    fts_opts[#fts_opts + 1] = "tokenize='" .. tokenize .. "'"

    if opts.content then
        validate_identifier(opts.content, "content table")
        fts_opts[#fts_opts + 1] = "content='" .. opts.content .. "'"
    end

    if opts.content_rowid then
        validate_identifier(opts.content_rowid, "content_rowid")
        fts_opts[#fts_opts + 1] = "content_rowid='" .. opts.content_rowid .. "'"
    end

    local sql = "CREATE VIRTUAL TABLE IF NOT EXISTS " .. tbl ..
                " USING fts5(" .. table.concat(fts_opts, ", ") .. ")"
    db.exec(sql)
end

--- Drop an FTS5 index. Idempotent.
-- @function search.drop_index
-- @tparam string name
function search.drop_index(name)
    require_sqlite()
    validate_identifier(name, "index name")
    db.exec("DROP TABLE IF EXISTS " .. fts_table(name))
end

--- Insert or replace a document in the index.
--
-- @function search.index
-- @tparam string name    Index name.
-- @tparam string|number id  Document identifier.
-- @tparam table  fields  Map of column name → value (e.g. `{title=..., body=...}`).
-- @raise If `id` is nil or any field name fails validation.
function search.index(name, id, fields)
    require_sqlite()
    validate_identifier(name, "index name")
    if id == nil then
        error("search.index: id must not be nil")
    end
    if type(fields) ~= "table" then
        error("search.index: fields must be a table")
    end

    local tbl = fts_table(name)
    local col_names = { "id" }
    local placeholders = { "?" }
    local values = { id }

    for col, val in pairs(fields) do
        validate_identifier(col, "field name")
        col_names[#col_names + 1] = col
        placeholders[#placeholders + 1] = "?"
        values[#values + 1] = val
    end

    local sql = "INSERT OR REPLACE INTO " .. tbl ..
                "(" .. table.concat(col_names, ", ") .. ") VALUES (" ..
                table.concat(placeholders, ", ") .. ")"
    db.exec(sql, values)
end

--- Remove a document from the index by id.
-- @function search.remove
-- @tparam string name
-- @tparam string|number id
function search.remove(name, id)
    require_sqlite()
    validate_identifier(name, "index name")
    if id == nil then
        error("search.remove: id must not be nil")
    end

    db.exec("DELETE FROM " .. fts_table(name) .. " WHERE id = ?", { id })
end

--- Query the FTS5 index.
--
-- @function search.query
-- @tparam string name
-- @tparam string query  FTS5 MATCH expression (e.g. `"hello world"`,
--   `"title:hello AND body:world"`).
-- @tparam[opt] table opts
-- @tparam[opt=20] number opts.limit
-- @tparam[opt=0]  number opts.offset
-- @tparam[opt] table opts.snippet
--   `{ column = N, tokens = N, before = "<b>", after = "</b>", ellipsis = "..." }`
--   Adds a `snippet` column to each result row.
-- @tparam[opt] table opts.highlight
--   `{ column = N, before = "<b>", after = "</b>" }` —
--   adds a `highlight` column.
-- @tparam[opt="rank"] string opts.order  `"rank"` or `"rowid"`.
-- @treturn {table,...}  Array of `{id, rank, snippet?, highlight?}` rows.
function search.query(name, query, opts)
    require_sqlite()
    validate_identifier(name, "index name")
    if type(query) ~= "string" or query == "" then
        error("search.query: query must be a non-empty string")
    end
    opts = opts or {}

    -- Bound the FTS5 MATCH expression. The query is safely `?`-parameterized
    -- (no SQL injection), but a pathological expression (thousands of OR terms,
    -- deeply nested NEAR/parentheses) is a CPU-amplification DoS when a public
    -- search box forwards untrusted `req.query.q` verbatim. Cap length and
    -- boolean-operator/paren count; callers that legitimately need more can
    -- raise opts.max_query_len / opts.max_query_terms.
    local max_len = opts.max_query_len or 1024
    local max_terms = opts.max_query_terms or 64
    if #query > max_len then
        error("search.query: query too long (" .. #query .. " > " .. max_len .. ")")
    end
    local _, term_count = query:gsub("[%(%)]", "")           -- parentheses
    local _, or_count = query:gsub("%f[%w]OR%f[%W]", "")     -- OR operators
    local _, near_count = query:gsub("%f[%w]NEAR%f[%W]", "") -- NEAR operators
    if term_count + or_count + near_count > max_terms then
        error("search.query: query too complex (>" .. max_terms
              .. " operators/groups); bound untrusted input")
    end

    local tbl = fts_table(name)
    local limit = opts.limit or 20
    local offset = opts.offset or 0

    -- Build SELECT columns and params
    local select_cols = { "id" }
    local params = {}

    if opts.snippet then
        local s = opts.snippet
        local col = math.floor(tonumber(s.column) or 1)
        if col < 0 then error("search: snippet column must be non-negative") end
        local tokens = tonumber(s.tokens) or 32
        select_cols[#select_cols + 1] = "snippet(" .. tbl .. ", " ..
            tostring(col) .. ", ?, ?, ?, " .. tostring(tokens) .. ") AS snippet"
        params[#params + 1] = s.before or "<b>"
        params[#params + 1] = s.after or "</b>"
        params[#params + 1] = s.ellipsis or "..."
    elseif opts.highlight then
        local h = opts.highlight
        local col = math.floor(tonumber(h.column) or 1)
        if col < 0 then error("search: highlight column must be non-negative") end
        select_cols[#select_cols + 1] = "highlight(" .. tbl .. ", " ..
            tostring(col) .. ", ?, ?) AS highlight"
        params[#params + 1] = h.before or "<b>"
        params[#params + 1] = h.after or "</b>"
    end

    select_cols[#select_cols + 1] = "rank"

    -- Build ORDER BY
    local order = "rank"
    if opts.order == "rowid" then
        order = "rowid"
    end

    local sql = "SELECT " .. table.concat(select_cols, ", ") ..
                " FROM " .. tbl ..
                " WHERE " .. tbl .. " MATCH ?" ..
                " ORDER BY " .. order ..
                " LIMIT ? OFFSET ?"

    -- MATCH param, then LIMIT, OFFSET
    params[#params + 1] = query
    params[#params + 1] = limit
    params[#params + 1] = offset

    return db.query(sql, params)
end

--- Bulk re-index from a source table.
--
-- Clears every row in `_hull_fts_<name>` then `INSERT INTO ... SELECT
-- FROM <source_table>` for an atomic-feeling rebuild.
--
-- @function search.reindex
-- @tparam string name
-- @tparam string source_table  Source SQL table to read from.
-- @tparam[opt] table opts
-- @tparam[opt] table opts.columns
--   Map of `<fts_column> -> <source_column>` (e.g. `{ title = "title",
--   body = "content" }`).
-- @tparam[opt="id"] string opts.id_column  Source table's ID column.
function search.reindex(name, source_table, opts)
    require_sqlite()
    validate_identifier(name, "index name")
    validate_identifier(source_table, "source table")
    opts = opts or {}

    local tbl = fts_table(name)
    local id_column = opts.id_column or "id"
    validate_identifier(id_column, "id_column")

    -- Build column mappings
    local fts_cols = { "id" }
    local src_cols = { id_column }

    if opts.columns then
        for fts_col, src_col in pairs(opts.columns) do
            validate_identifier(fts_col, "FTS column")
            validate_identifier(src_col, "source column")
            fts_cols[#fts_cols + 1] = fts_col
            src_cols[#src_cols + 1] = src_col
        end
    end

    -- Clear existing index data
    db.exec("DELETE FROM " .. tbl)

    -- Bulk insert from source table
    local sql = "INSERT INTO " .. tbl ..
                "(" .. table.concat(fts_cols, ", ") .. ")" ..
                " SELECT " .. table.concat(src_cols, ", ") ..
                " FROM " .. source_table
    db.exec(sql)
end

return search
