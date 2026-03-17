--
-- hull.search -- FTS5 full-text search wrapper
--
-- Provides full-text search indexing and querying backed by SQLite FTS5
-- virtual tables. Uses the global `db` object for storage.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local search = {}

-- ── Helpers ───────────────────────────────────────────────────────────

local FTS_PREFIX = "_hull_fts_"

--- Validate an SQL identifier (table name, column name).
-- Must match [a-zA-Z_][a-zA-Z0-9_]* and must not start with _hull_.
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

--- Create an FTS5 virtual table for full-text search.
-- columns: array of column name strings (e.g. {"title", "body"})
-- opts.tokenize: FTS5 tokenizer (default "unicode61")
-- opts.content: external content table name
-- opts.content_rowid: rowid column name for external content
function search.create_index(name, columns, opts)
    validate_identifier(name, "index name")
    validate_columns(columns)
    opts = opts or {}

    local tbl = fts_table(name)
    local col_list = "id, " .. table.concat(columns, ", ")

    -- Build FTS5 options
    local fts_opts = {}
    fts_opts[#fts_opts + 1] = col_list

    local tokenize = opts.tokenize or "unicode61"
    if not tokenize:match("^[a-zA-Z0-9_ ]+$") then
        error("search: invalid tokenize option (must match [a-zA-Z0-9_ ]+)")
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

--- Drop an FTS5 index.
function search.drop_index(name)
    validate_identifier(name, "index name")
    db.exec("DROP TABLE IF EXISTS " .. fts_table(name))
end

--- Insert or replace a document in the index.
-- name: index name
-- id: document identifier (string or number)
-- fields: table mapping column names to values (e.g. {title="Hello", body="World"})
function search.index(name, id, fields)
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
function search.remove(name, id)
    validate_identifier(name, "index name")
    if id == nil then
        error("search.remove: id must not be nil")
    end

    db.exec("DELETE FROM " .. fts_table(name) .. " WHERE id = ?", { id })
end

--- Query the FTS5 index.
-- name: index name
-- query: FTS5 match expression (e.g. "hello world", "title:hello")
-- opts.limit: max results (default 20)
-- opts.offset: result offset (default 0)
-- opts.snippet: table with {column, tokens, before, after, ellipsis}
-- opts.highlight: table with {column, before, after}
-- opts.order: "rank" (default) or "rowid"
function search.query(name, query, opts)
    validate_identifier(name, "index name")
    if type(query) ~= "string" or query == "" then
        error("search.query: query must be a non-empty string")
    end
    opts = opts or {}

    local tbl = fts_table(name)
    local limit = opts.limit or 20
    local offset = opts.offset or 0

    -- Build SELECT columns and params
    local select_cols = { "id" }
    local params = {}

    if opts.snippet then
        local s = opts.snippet
        local col = tonumber(s.column) or 1
        local tokens = tonumber(s.tokens) or 32
        select_cols[#select_cols + 1] = "snippet(" .. tbl .. ", " ..
            tostring(col) .. ", ?, ?, ?, " .. tostring(tokens) .. ") AS snippet"
        params[#params + 1] = s.before or "<b>"
        params[#params + 1] = s.after or "</b>"
        params[#params + 1] = s.ellipsis or "..."
    elseif opts.highlight then
        local h = opts.highlight
        local col = tonumber(h.column) or 1
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
-- name: index name
-- source_table: table to read documents from
-- opts.columns: table mapping FTS column names to source column names
--               e.g. {title = "title", body = "content"}
-- opts.id_column: source table's ID column (default "id")
function search.reindex(name, source_table, opts)
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
