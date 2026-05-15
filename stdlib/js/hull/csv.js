/*
 * hull:csv -- RFC 4180 CSV parser and writer
 *
 * csv.parse(text, opts?)  -> array of row arrays (or row objects if headers=true)
 * csv.encode(rows, opts?) -> CSV string
 *
 * Options:
 *   headers   (bool,   default false) - first row is header row
 *   separator (string, default ",")   - field delimiter
 *   quote     (string, default '"')   - quote character
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

function parse(text, opts) {
    if (typeof text !== "string" || text.length === 0) return [];

    const sep = (opts && opts.separator) || ",";
    const quo = (opts && opts.quote) || '"';
    const useHeaders = !!(opts && opts.headers);
    // L-3: accept Lua-style `max_rows` as well as the canonical
    // `maxRows`. `??` preserves an explicit 0 (the prior `||` would
    // have replaced it with the default).
    const maxRows = (opts && (opts.maxRows ?? opts.max_rows)) ?? 100000;

    const rows = [];
    let row = [];
    let field = "";
    let inQuoted = false;
    let i = 0;
    const len = text.length;

    while (i < len) {
        if (rows.length >= maxRows)
            throw new Error("csv.parse: exceeded maxRows limit (" + maxRows + ")");
        const ch = text[i];

        if (inQuoted) {
            if (ch === quo) {
                // Look ahead: doubled quote is an escape
                if (i + 1 < len && text[i + 1] === quo) {
                    field += quo;
                    i += 2;
                } else {
                    // End of quoted field
                    inQuoted = false;
                    i++;
                }
            } else {
                // Accumulate character (including newlines inside quotes)
                field += ch;
                i++;
            }
        } else {
            if (ch === quo && field.length === 0) {
                // Start of quoted field (quote must be at field start)
                inQuoted = true;
                i++;
            } else if (ch === sep) {
                // Field delimiter
                row.push(field);
                field = "";
                i++;
            } else if (ch === "\r") {
                // CRLF or bare CR -> end of row
                row.push(field);
                field = "";
                rows.push(row);
                row = [];
                i++;
                if (i < len && text[i] === "\n") i++;
            } else if (ch === "\n") {
                // LF -> end of row
                row.push(field);
                field = "";
                rows.push(row);
                row = [];
                i++;
            } else {
                field += ch;
                i++;
            }
        }
    }

    // Handle last field/row (no trailing newline case)
    // If there is any pending field content or the row has fields,
    // or the text did not end with a newline, push the final row.
    if (field.length > 0 || row.length > 0) {
        row.push(field);
        rows.push(row);
    }

    if (rows.length === 0) return [];

    if (useHeaders) {
        const headers = rows[0];
        const result = [];
        for (let r = 1; r < rows.length; r++) {
            const obj = {};
            for (let c = 0; c < headers.length; c++) {
                const key = headers[c];
                if (typeof key !== "string" || key.length === 0) continue;
                obj[key] = c < rows[r].length ? rows[r][c] : "";
            }
            result.push(obj);
        }
        return result;
    }

    return rows;
}

function encode(rows, opts) {
    if (!Array.isArray(rows) || rows.length === 0) return "";

    const sep = (opts && opts.separator) || ",";
    const quo = (opts && opts.quote) || '"';
    const useHeaders = !!(opts && opts.headers);
    const doubledQuote = quo + quo;

    function needsQuoting(value) {
        for (let i = 0; i < value.length; i++) {
            const ch = value[i];
            if (ch === sep || ch === quo || ch === "\n" || ch === "\r")
                return true;
        }
        return false;
    }

    function quoteField(value) {
        const s = String(value);
        if (needsQuoting(s)) {
            const parts = [];
            for (let i = 0; i < s.length; i++) {
                if (s[i] === quo) parts.push(doubledQuote);
                else parts.push(s[i]);
            }
            return quo + parts.join("") + quo;
        }
        return s;
    }

    function encodeRow(fields) {
        const parts = [];
        for (let i = 0; i < fields.length; i++) {
            parts.push(quoteField(fields[i] === null || fields[i] === undefined ? "" : fields[i]));
        }
        return parts.join(sep);
    }

    let out = "";

    if (useHeaders) {
        // rows is array of objects; derive headers from first object keys
        if (typeof rows[0] !== "object" || rows[0] === null) return "";

        const headers = Object.keys(rows[0]);
        out += encodeRow(headers) + "\n";

        for (let r = 0; r < rows.length; r++) {
            const values = [];
            for (let c = 0; c < headers.length; c++) {
                values.push(rows[r][headers[c]]);
            }
            out += encodeRow(values) + "\n";
        }
    } else {
        // rows is array of arrays
        for (let r = 0; r < rows.length; r++) {
            out += encodeRow(rows[r]) + "\n";
        }
    }

    return out;
}

const csv = { parse, encode };
export { csv };
