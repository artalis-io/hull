/*
 * hull:search -- SQLite FTS5 full-text search wrapper
 *
 * search.createIndex(name, columns, opts?)   - creates FTS5 virtual table
 * search.dropIndex(name)                     - drops FTS5 table
 * search.index(name, id, fields)             - insert/replace document
 * search.remove(name, id)                    - delete document
 * search.query(name, query, opts?)           - returns array of {id, ...columns, rank}
 * search.reindex(name, sourceTable, opts?)   - bulk re-index from source table
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { db } from "hull:db";

const IDENT_RE = /^[a-zA-Z_][a-zA-Z0-9_]*$/;
const TABLE_PREFIX = "_hull_fts_";

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
 * Create an FTS5 virtual table.
 *
 * @param {string} name - index name (becomes _hull_fts_<name>)
 * @param {string[]} columns - column names for the FTS index
 * @param {Object} opts - optional: tokenize, content, contentRowid
 */
function createIndex(name, columns, opts) {
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

    const tokenize = o.tokenize || "unicode61";
    if (!/^[a-zA-Z0-9_ ]+$/.test(tokenize))
        throw new Error("search: invalid tokenize option (must match [a-zA-Z0-9_ ]+)");
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
 * Drop an FTS5 virtual table.
 *
 * @param {string} name - index name
 */
function dropIndex(name) {
    validateIdent(name, "index name");
    db.exec("DROP TABLE IF EXISTS " + ftsTable(name));
}

/**
 * Insert or replace a document in the FTS index.
 *
 * @param {string} name - index name
 * @param {*} id - document ID (string or number)
 * @param {Object} fields - object mapping column names to values
 */
function index(name, id, fields) {
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
 * Delete a document from the FTS index.
 *
 * @param {string} name - index name
 * @param {*} id - document ID
 */
function remove(name, id) {
    validateIdent(name, "index name");

    if (id === undefined || id === null)
        throw new Error("id is required");

    db.exec(
        "DELETE FROM " + ftsTable(name) + " WHERE id = ?",
        [id]
    );
}

/**
 * Query the FTS index.
 *
 * @param {string} name - index name
 * @param {string} q - FTS5 query string
 * @param {Object} opts - optional: limit, offset, snippet, highlight, order
 * @returns {Array} array of result objects with id, columns, and rank
 */
function query(name, q, opts) {
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
 * Bulk re-index from a source table into the FTS index.
 *
 * @param {string} name - index name
 * @param {string} sourceTable - source table name
 * @param {Object} opts - optional: columns (map of FTS col -> source col), idColumn
 */
function reindex(name, sourceTable, opts) {
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
