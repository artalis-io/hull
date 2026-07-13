/**
 * @file hull:search
 * @module hull:search
 * @description Full-text search backed by SQLite FTS5. Lua parity:
 *   `hull.search`.
 *
 * Each search index is a `_hull_fts_<name>` FTS5 virtual table with a
 * fixed `id` column plus the columns the caller declared. Identifiers
 * are validated against `/^[A-Za-z_][A-Za-z0-9_]*$/` and rejected if
 * they begin with `_hull_`.
 *
 * Supports snippet/highlight extraction and bulk re-indexing from a
 * source table.
 *
 * @license AGPL-3.0-or-later
 *
 * @example
 * search.createIndex("posts", ["title", "body"]);
 * search.index("posts", post.id, { title: post.title, body: post.body });
 * for (const hit of search.query("posts", "hull AND wasm", { limit: 10 })) {
 *     console.log(hit.id, hit.rank);
 * }
 */

import { db } from "hull:db";

const IDENT_RE = /^[a-zA-Z_][a-zA-Z0-9_]*$/;
const TABLE_PREFIX = "_hull_fts_";

// FTS5 is a SQLite-only feature; there is no Postgres equivalent wired
// here. Fail with a clear message rather than a cryptic SQL error when
// the default connection is not SQLite. Same policy as db.udf.
function requireSqlite() {
    if (db.backendName !== "sqlite") {
        throw new Error(
            "hull/search requires the SQLite backend (FTS5); the current " +
            "database connection is '" + db.backendName + "'. Full-text " +
            "search is not available on this backend.");
    }
}

/**
 * Validate an identifier (table name, column name).
 * Must match /^[a-zA-Z_][a-zA-Z0-9_]*$/ and must not start with _hull_.
 * @param {string} name - identifier to validate
 * @param {string} label - description for error messages
 */
function validateIdent(name, label) {
    if (typeof name !== "string" || !IDENT_RE.test(name))
        throw new Error(label + " must match /^[a-zA-Z_][a-zA-Z0-9_]*$/: " + String(name));
    if (name.indexOf("_hull_") === 0)
        throw new Error(label + " must not start with '_hull_': " + name);
}

/**
 * Build the internal FTS5 table name from a user-facing index name.
 */
function ftsTable(name) {
    return TABLE_PREFIX + name;
}

/**
 * Create an FTS5 virtual table for full-text search. Idempotent.
 *
 * @param {string}   name             Index name (becomes `_hull_fts_<name>`).
 * @param {string[]} columns          Searchable column names (≥ 1).
 * @param {Object}   [opts]
 * @param {string}   [opts.tokenize="unicode61"]  FTS5 tokenizer spec.
 * @param {string}   [opts.content]               External content table name.
 * @param {string}   [opts.contentRowid]          Rowid column for external content.
 * @throws If `name`/columns fail identifier validation.
 */
function createIndex(name, columns, opts) {
    requireSqlite();
    validateIdent(name, "index name");

    if (!Array.isArray(columns) || columns.length === 0)
        throw new Error("columns must be a non-empty array");

    for (let i = 0; i < columns.length; i++)
        validateIdent(columns[i], "column name");

    const o = opts || {};
    const table = ftsTable(name);

    // Build column list: id + user columns
    let colDefs = "id";
    for (let j = 0; j < columns.length; j++)
        colDefs += ", " + columns[j];

    // FTS5 options
    let ftsOpts = "";

    // Only default when `tokenize` was omitted. An explicit "" must
    // fall through to the validator so Lua and JS agree byte-for-byte
    // — Lua's `or` keeps "" because Lua strings are always truthy.
    const tokenize = (o.tokenize === undefined || o.tokenize === null)
        ? "unicode61"
        : o.tokenize;
    // FTS5 tokenize names are short identifiers ("unicode61", "porter ascii",
    // "trigram") optionally followed by identifier-or-integer args
    // ("unicode61 remove_diacritics 1"). First token must be an identifier;
    // subsequent tokens may also be positive integers. Reject leading/
    // trailing/double spaces or anything else that could escape the quoted
    // SQL string. Mirrors the Lua sibling's grammar exactly.
    if (!/^[a-zA-Z][a-zA-Z0-9_]*( ([a-zA-Z][a-zA-Z0-9_]*|[0-9]+))*$/.test(tokenize))
        throw new Error("search: invalid tokenize option");
    ftsOpts += ", tokenize=\"" + tokenize + "\"";

    if (o.content) {
        validateIdent(o.content, "content table name");
        ftsOpts += ", content=\"" + o.content + "\"";
    }

    if (o.contentRowid) {
        validateIdent(o.contentRowid, "content rowid column");
        ftsOpts += ", content_rowid=\"" + o.contentRowid + "\"";
    }

    db.exec(
        "CREATE VIRTUAL TABLE IF NOT EXISTS " + table +
        " USING fts5(" + colDefs + ftsOpts + ")"
    );
}

/**
 * Drop an FTS5 index. Idempotent.
 * @param {string} name
 */
function dropIndex(name) {
    requireSqlite();
    validateIdent(name, "index name");
    db.exec("DROP TABLE IF EXISTS " + ftsTable(name));
}

/**
 * Insert or replace a document in the index.
 *
 * @param {string}        name    Index name.
 * @param {string|number} id      Document identifier.
 * @param {Object}        fields  Map of column name → value.
 * @throws If `id` is `null`/`undefined` or any field name fails validation.
 */
function index(name, id, fields) {
    requireSqlite();
    validateIdent(name, "index name");

    if (id === undefined || id === null)
        throw new Error("id is required");
    if (!fields || typeof fields !== "object")
        throw new Error("fields must be an object");

    let keys = Object.keys(fields);
    if (keys.length === 0)
        throw new Error("fields must have at least one column");

    for (let i = 0; i < keys.length; i++)
        validateIdent(keys[i], "field name");

    const table = ftsTable(name);

    // Build INSERT OR REPLACE with column names in DDL, values parameterized
    let colList = "id";
    let placeholders = "?";
    let values = [id];

    for (let j = 0; j < keys.length; j++) {
        colList += ", " + keys[j];
        placeholders += ", ?";
        values.push(fields[keys[j]] != null ? String(fields[keys[j]]) : "");
    }

    // Delete existing row with same id first (FTS5 doesn't support OR REPLACE)
    db.exec(
        "DELETE FROM " + table + " WHERE id = ?",
        [id]
    );

    db.exec(
        "INSERT INTO " + table + " (" + colList + ") VALUES (" + placeholders + ")",
        values
    );
}

/**
 * Remove a document from the index by id.
 * @param {string}        name
 * @param {string|number} id
 */
function remove(name, id) {
    requireSqlite();
    validateIdent(name, "index name");

    if (id === undefined || id === null)
        throw new Error("id is required");

    db.exec(
        "DELETE FROM " + ftsTable(name) + " WHERE id = ?",
        [id]
    );
}

/**
 * Query the FTS5 index.
 *
 * @param {string} name
 * @param {string} q     FTS5 MATCH expression
 *   (e.g. `"hello world"`, `"title:hello AND body:world"`).
 * @param {Object} [opts]
 * @param {number} [opts.limit=20]
 * @param {number} [opts.offset=0]
 * @param {{column:number,tokens:number,before:string,after:string,ellipsis:string}}
 *                 [opts.snippet]
 * @param {{column:number,before:string,after:string}} [opts.highlight]
 * @param {("rank"|"rowid")} [opts.order="rank"]
 * @returns {Array<{id:any, rank:number, snippet?:string, highlight?:string}>}
 */
function query(name, q, opts) {
    requireSqlite();
    validateIdent(name, "index name");

    if (typeof q !== "string" || q.length === 0)
        throw new Error("query string is required");

    const o = opts || {};
    const table = ftsTable(name);
    let limit = o.limit !== undefined ? o.limit : 20;
    let offset = o.offset !== undefined ? o.offset : 0;

    // Build SELECT columns and collect snippet/highlight params
    let selectCols = "id, rank";
    let snippetParams = null;
    let highlightParams = null;

    if (o.snippet) {
        const s = o.snippet;
        const sCol = Number(s.column !== undefined ? s.column : 0);
        if (!Number.isInteger(sCol) || sCol < 0)
            throw new Error("search: snippet column must be a non-negative integer");
        const sBefore = s.before || "<b>";
        const sAfter = s.after || "</b>";
        const sEllipsis = s.ellipsis || "...";
        const sTokens = s.tokens !== undefined ? s.tokens : 32;
        selectCols += ", snippet(" + table + ", " + sCol +
            ", ?, ?, ?, ?) AS snippet";
        snippetParams = [sBefore, sAfter, sEllipsis, sTokens];
    }

    if (o.highlight) {
        const h = o.highlight;
        const hCol = Number(h.column !== undefined ? h.column : 0);
        if (!Number.isInteger(hCol) || hCol < 0)
            throw new Error("search: highlight column must be a non-negative integer");
        const hBefore = h.before || "<b>";
        const hAfter = h.after || "</b>";
        selectCols += ", highlight(" + table + ", " + hCol +
            ", ?, ?) AS highlight";
        highlightParams = [hBefore, hAfter];
    }

    let orderBy = "rank";
    if (o.order === "rowid")
        orderBy = "rowid";

    const sql = "SELECT " + selectCols + " FROM " + table +
        " WHERE " + table + " MATCH ? ORDER BY " + orderBy +
        " LIMIT ? OFFSET ?";

    // Build parameter array (snippet/highlight params first, in SELECT order)
    const params = [];

    if (snippetParams)
        for (let k = 0; k < snippetParams.length; k++)
            params.push(snippetParams[k]);

    if (highlightParams)
        for (let k = 0; k < highlightParams.length; k++)
            params.push(highlightParams[k]);

    // MATCH parameter
    params.push(q);

    // LIMIT and OFFSET
    params.push(limit);
    params.push(offset);

    return db.query(sql, params);
}

/**
 * Bulk re-index from a source table.
 *
 * Clears every row in `_hull_fts_<name>` then `INSERT INTO ... SELECT
 * FROM <sourceTable>` for an atomic-feeling rebuild.
 *
 * @param {string} name
 * @param {string} sourceTable
 * @param {Object} opts
 * @param {Object<string,string>} opts.columns
 *   Map of `<ftsColumn> -> <sourceColumn>` (e.g. `{ title: "title",
 *   body: "content" }`).
 * @param {string} [opts.idColumn="id"]
 */
function reindex(name, sourceTable, opts) {
    requireSqlite();
    validateIdent(name, "index name");
    validateIdent(sourceTable, "source table name");

    const o = opts || {};
    let idCol = o.idColumn || "id";
    validateIdent(idCol, "id column name");

    const table = ftsTable(name);

    // Determine column mapping
    let columnMap = o.columns || {};
    let ftsColumns = Object.keys(columnMap);

    if (ftsColumns.length === 0)
        throw new Error("opts.columns is required: maps FTS columns to source columns");

    // Validate all column names
    for (let i = 0; i < ftsColumns.length; i++) {
        validateIdent(ftsColumns[i], "FTS column name");
        validateIdent(columnMap[ftsColumns[i]], "source column name");
    }

    // Build INSERT ... SELECT
    let ftsColList = "id";
    let srcColList = idCol;
    for (let j = 0; j < ftsColumns.length; j++) {
        ftsColList += ", " + ftsColumns[j];
        srcColList += ", " + columnMap[ftsColumns[j]];
    }

    // Clear and repopulate
    db.exec("DELETE FROM " + table);
    db.exec(
        "INSERT INTO " + table + " (" + ftsColList + ") " +
        "SELECT " + srcColList + " FROM " + sourceTable
    );
}

const search = { createIndex, dropIndex, index, remove, query, reindex };
export { search };
