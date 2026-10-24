/**
 * @file hull:search
 * @module hull:search
 * @description Full-text search backed by SQLite FTS5. Lua parity:
 *   `hull.search`.
 *
 * Each search index is a `_hull_fts_<name>` FTS5 virtual table with a
 * fixed `id` column plus the columns the caller declared. Identifiers
 * are validated against `/^[A-Za-z_][A-Za-z0-9_]*$/` and rejected if
 * they begin with `_hull_` or are a SQL keyword.
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

import { db as dbModule } from "hull:db";
const db = dbModule.default();
const IDENT_RE = /^[a-zA-Z_][a-zA-Z0-9_]*$/;
const TABLE_PREFIX = "_hull_fts_";

// An identifier that passes IDENT_RE cannot inject -- it carries no quote,
// space, semicolon or comment marker. What it CAN do is collide with SQL's
// own grammar: `sourceTable` and every column name below are interpolated
// UNPREFIXED, so an index re-built `FROM order`, or a column named `from`,
// parses as a keyword and fails deep inside SQLite with a syntax error that
// names neither the caller nor the offending word. Rejecting the keyword up
// front is what turns that into a diagnosable message. Mirrors the
// SQL_KEYWORDS check in search.lua -- same 27 words, same reason.
const SQL_KEYWORDS = new Set([
    "SELECT", "INSERT", "UPDATE", "DELETE", "DROP", "CREATE", "ALTER",
    "TABLE", "INDEX", "WHERE", "FROM", "INTO", "VALUES", "SET",
    "UNION", "JOIN", "ORDER", "GROUP", "HAVING", "LIMIT", "OFFSET",
    "BEGIN", "COMMIT", "ROLLBACK", "PRAGMA", "ATTACH", "DETACH",
]);

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
 * Must match /^[a-zA-Z_][a-zA-Z0-9_]*$/, must not start with _hull_, and
 * must not be a SQL keyword.
 * @param {string} name - identifier to validate
 * @param {string} label - description for error messages
 */
function validateIdent(name, label) {
    if (typeof name !== "string" || !IDENT_RE.test(name))
        throw new Error(label + " must match /^[a-zA-Z_][a-zA-Z0-9_]*$/: " + String(name));
    // Case-insensitive: SQLite identifiers are, and this module's SQL runs as
    // a stdlib caller, past the C-level _hull_* guard. "_HULL_SESSIONS"
    // passed the old check, so reindex() copied every session row into an
    // index the app could query.
    if (name.slice(0, 6).toLowerCase() === "_hull_")
        throw new Error(label + " must not start with '_hull_': " + name);
    if (SQL_KEYWORDS.has(name.toUpperCase()))
        throw new Error(label + " must not be a SQL keyword: " + name);
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

    if (!Array.isArray(columns))
        throw new Error("columns must be a non-empty array");

    // Every caller value is read ONCE, into a local that is validated and
    // then built into the SQL: this SQL runs from a stdlib frame, past the
    // C-level _hull_* guard, and a getter or Proxy answered the validation
    // with one name and the SQL with another (audit 19).
    const n = columns.length;
    if (!(n > 0))
        throw new Error("columns must be a non-empty array");
    const cols = [];
    for (let i = 0; i < n; i++) {
        const c = columns[i];
        validateIdent(c, "column name");
        cols[i] = c;
    }

    const o = opts || {};
    const table = ftsTable(name);

    // Build column list: id + user columns
    let colDefs = "id";
    for (let j = 0; j < cols.length; j++)
        colDefs += ", " + cols[j];

    // FTS5 options
    let ftsOpts = "";

    // Only default when `tokenize` was omitted. An explicit "" must
    // fall through to the validator so Lua and JS agree byte-for-byte
    // - Lua's `or` keeps "" because Lua strings are always truthy.
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

    const content = o.content;
    if (content) {
        validateIdent(content, "content table name");
        ftsOpts += ", content=\"" + content + "\"";
    }

    const contentRowid = o.contentRowid;
    if (contentRowid) {
        validateIdent(contentRowid, "content rowid column");
        ftsOpts += ", content_rowid=\"" + contentRowid + "\"";
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
 *   The indexed text in `snippet` / `highlight` is HTML-escaped; only the
 *   before / after / ellipsis markers are inserted as given, so the result
 *   is safe to render raw (audit 26).
 * @param {("rank"|"rowid")} [opts.order="rank"]
 * @returns {Array<{id:any, rank:number, snippet?:string, highlight?:string}>}
 */
// snippet() / highlight() return the INDEXED text, unescaped, with the
// markers spliced in: a document holding `<img src=x onerror=...>` came back
// as markup, and the `<b>` default forced apps to render the result raw
// (stored XSS; audit 26). So FTS5 is handed private-use sentinels as the
// markers, the text is HTML-escaped, and the sentinels are then replaced by
// the caller's before / after / ellipsis (app-chosen, trusted markup). A
// document that itself contains U+E000-U+E002 gets an extra caller marker
// there - still only the app's own markup, though it can unbalance tags.
const MARK_BEFORE = "", MARK_AFTER = "", MARK_ELLIPSIS = "";

function escapeMarked(text, before, after, ellipsis) {
    if (typeof text !== "string") return text;
    return text.replace(/[&<>"'-]/g, (c) =>
        c === MARK_BEFORE ? before
            : c === MARK_AFTER ? after
            : c === MARK_ELLIPSIS ? (ellipsis === null ? c : ellipsis)
            : c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;"
            : c === "\"" ? "&quot;" : "&#39;");
}

// Integer option in [min, max], or `def` when undefined. A string, NaN or a
// fraction is refused: "-1" bound as LIMIT returned every row, and a string
// cap made the query bounds compare false (audit 26).
function intOpt(o, key, def, min, max) {
    const v = o[key];
    if (v === undefined) return def;
    if (typeof v !== "number" || !Number.isInteger(v) || v < min || v > max)
        throw new Error("search.query: " + key + " must be an integer in ["
                        + min + ", " + max + "]");
    return v;
}

const SEARCH_MAX_LIMIT = 1000;
const SEARCH_MAX_OFFSET = 2147483647;

function query(name, q, opts) {
    requireSqlite();
    validateIdent(name, "index name");

    if (typeof q !== "string" || q.length === 0)
        throw new Error("query string is required");

    const o = opts || {};

    // Bound the FTS5 MATCH expression (mirrors search.lua). The query is safely
    // `?`-parameterized (no injection), but a pathological expression (thousands
    // of OR terms, deeply nested NEAR/parens) is a CPU-amplification DoS when a
    // public search box forwards untrusted input verbatim.
    const maxLen = intOpt(o, "maxQueryLen", 1024, 1, 1048576);
    const maxTerms = intOpt(o, "maxQueryTerms", 64, 1, 65536);
    const limit = intOpt(o, "limit", 20, 1, SEARCH_MAX_LIMIT);
    const offset = intOpt(o, "offset", 0, 0, SEARCH_MAX_OFFSET);
    if (q.length > maxLen)
        throw new Error("query too long (" + q.length + " > " + maxLen + ")");
    // Every word is a term (implicit AND costs as much as OR), every prefix
    // `*` widens one, and parentheses group: count all three. Counting only
    // OR / NEAR / parens let "a b c d ..." or "a* b* c* ..." through.
    const complexity =
        (q.match(/[()]/g) || []).length +
        (q.match(/[^\s()]+/g) || []).length +
        (q.match(/\*/g) || []).length;
    if (complexity > maxTerms)
        throw new Error("query too complex (>" + maxTerms +
                        " operators/groups); bound untrusted input");
    const table = ftsTable(name);

    // Build SELECT columns and collect snippet/highlight params
    let selectCols = "id, rank";
    let snippetParams = null;
    let highlightParams = null;
    let snippetMarks = null;
    let highlightMarks = null;

    if (o.snippet) {
        const s = o.snippet;
        const sCol = Number(s.column !== undefined ? s.column : 0);
        if (!Number.isInteger(sCol) || sCol < 0)
            throw new Error("search: snippet column must be a non-negative integer");
        const sBefore = String(s.before || "<b>");
        const sAfter = String(s.after || "</b>");
        const sEllipsis = String(s.ellipsis || "...");
        const sTokens = s.tokens !== undefined ? s.tokens : 32;
        selectCols += ", snippet(" + table + ", " + sCol +
            ", ?, ?, ?, ?) AS snippet";
        snippetParams = [MARK_BEFORE, MARK_AFTER, MARK_ELLIPSIS, sTokens];
        snippetMarks = [sBefore, sAfter, sEllipsis];
    }

    if (o.highlight) {
        const h = o.highlight;
        const hCol = Number(h.column !== undefined ? h.column : 0);
        if (!Number.isInteger(hCol) || hCol < 0)
            throw new Error("search: highlight column must be a non-negative integer");
        const hBefore = String(h.before || "<b>");
        const hAfter = String(h.after || "</b>");
        selectCols += ", highlight(" + table + ", " + hCol +
            ", ?, ?) AS highlight";
        highlightParams = [MARK_BEFORE, MARK_AFTER];
        highlightMarks = [hBefore, hAfter];
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

    // A query FTS5 cannot parse is the user's input, not a server fault: it
    // returns no rows, with the reason as `.error` on the array, where it
    // used to throw (a 500 for any search box fed "foo AND"). Other errors
    // still throw.
    let rows;
    try {
        rows = db.query(sql, params);
    } catch (e) {
        const msg = String(e && e.message !== undefined ? e.message : e);
        if (/fts5:|syntax error|no such column|unterminated|malformed MATCH/.test(msg)) {
            const none = [];
            none.error = "invalid search query";
            return none;
        }
        throw e;
    }
    if (snippetMarks || highlightMarks) {
        for (let k = 0; k < rows.length; k++) {
            const row = rows[k];
            if (snippetMarks)
                row.snippet = escapeMarked(row.snippet, snippetMarks[0],
                                           snippetMarks[1], snippetMarks[2]);
            if (highlightMarks)
                row.highlight = escapeMarked(row.highlight, highlightMarks[0],
                                             highlightMarks[1], null);
        }
    }
    return rows;
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

    // Determine column mapping. Each source column is read ONCE and the
    // validated copy is what goes into the SQL: it runs from a stdlib frame,
    // past the C-level _hull_* guard, and a getter or Proxy that answered
    // the validation with one name and the SQL with another copied any
    // _hull_* table into an index the app could query (audit 19).
    const columnMap = o.columns || {};
    const ftsColumns = Object.keys(columnMap);

    if (ftsColumns.length === 0)
        throw new Error("opts.columns is required: maps FTS columns to source columns");

    const srcColumns = [];
    for (let i = 0; i < ftsColumns.length; i++) {
        validateIdent(ftsColumns[i], "FTS column name");
        const src = columnMap[ftsColumns[i]];
        validateIdent(src, "source column name");
        srcColumns[i] = src;
    }

    // Build INSERT ... SELECT
    let ftsColList = "id";
    let srcColList = idCol;
    for (let j = 0; j < ftsColumns.length; j++) {
        ftsColList += ", " + ftsColumns[j];
        srcColList += ", " + srcColumns[j];
    }

    // Clear and repopulate in one transaction: a failure part-way used to
    // leave the index empty, and a reader between the two saw no results.
    db.batch(() => {
        db.exec("DELETE FROM " + table);
        db.exec(
            "INSERT INTO " + table + " (" + ftsColList + ") " +
            "SELECT " + srcColList + " FROM " + sourceTable
        );
    });
}

const search = { createIndex, dropIndex, index, remove, query, reindex };
export { search };
