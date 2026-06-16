/**
 * @file hull:web:htmx:table
 * @module hull:web:htmx:table
 * @description HTMX table widget — schema-driven `<table>`
 * renderer that composes sort + inline-edit per column.
 *
 * The grid pattern of every admin / data UI in one helper. Takes
 * rows + a column schema; emits a complete `<table>` block with
 * sortable column headers (via hull:web:htmx:sort), inline-
 * editable cells (via hull:web:htmx:inline-edit), and plain text
 * cells for everything else (HTML-escaped).
 *
 * Apps wire search input + pagination footer around the table
 * separately — this widget owns the grid, not the page layout.
 *
 * Lua parity: same surface as hull.web.htmx.table.
 */

import { sort as sortWidget } from "hull:web:htmx:sort";
import { inlineEdit } from "hull:web:htmx:inline-edit";

const ESC = { "&": "&amp;", '"': "&quot;", "<": "&lt;", ">": "&gt;", "'": "&#39;" };
function esc(s) {
    return String(s == null ? "" : s).replace(/[&"<>']/g, (c) => ESC[c]);
}

/**
 * Render a sortable, inline-editable `<table>` from rows + schema.
 *
 * @param {object[]} rows
 * @param {object[]} schema  Array of column specs:
 *   { name, label, sortable?, editable?, render? }
 * @param {object} opts
 * @param {string} opts.baseUrl                Required. Base URL.
 * @param {string} [opts.target]               hx-target for sort
 *                                             swaps. Default
 *                                             "closest table".
 * @param {string} [opts.swap]                 hx-swap for sort
 *                                             swaps. When opts.target
 *                                             wraps table + nav +
 *                                             actions (e.g.
 *                                             "#grid"), pass
 *                                             "innerHTML". When
 *                                             targeting the table
 *                                             itself, pass
 *                                             "outerHTML".
 * @param {?object} [opts.currentSort]         Output of sort.parse.
 * @param {boolean} [opts.pushUrl]             For sort. Default true.
 * @param {function} [opts.editUrlFor]         (row, colName) → string.
 *                                             Required if any column
 *                                             has editable=true.
 * @param {string} [opts.emptyLabel]           Default "No rows.".
 * @returns {string}  Full `<table>...</table>` HTML.
 */
function render(rows, schema, opts) {
    opts = opts || {};
    if (!opts.baseUrl) throw new Error("table.render: opts.baseUrl is required");
    rows   = rows   || [];
    schema = schema || [];

    const parts = ['<table class="hull-table"><thead><tr>'];

    for (const col of schema) {
        if (col.sortable) {
            const attrs = sortWidget.headerAttrs(col.name, opts.currentSort, {
                url:     opts.baseUrl,
                target:  opts.target,
                swap:    opts.swap,
                pushUrl: opts.pushUrl,
            });
            parts.push(`<th ${attrs}>${esc(col.label || col.name)}</th>`);
        } else {
            parts.push(`<th>${esc(col.label || col.name)}</th>`);
        }
    }
    parts.push('</tr></thead><tbody>');

    if (rows.length === 0) {
        parts.push(
            `<tr class="hull-table-empty"><td colspan="${schema.length}">`
            + esc(opts.emptyLabel || "No rows.") + `</td></tr>`,
        );
    } else {
        for (const row of rows) {
            parts.push('<tr>');
            for (const col of schema) {
                const value = row[col.name];
                let cellHtml;
                if (col.render) {
                    cellHtml = col.render(value, row);
                } else if (col.editable) {
                    if (!opts.editUrlFor) {
                        throw new Error("table.render: opts.editUrlFor is required "
                            + "when any column has editable=true");
                    }
                    cellHtml = inlineEdit.cell({
                        value:   value,
                        editUrl: opts.editUrlFor(row, col.name),
                        label:   "Edit " + (col.label || col.name),
                    });
                } else {
                    cellHtml = esc(value);
                }
                parts.push(`<td>${cellHtml}</td>`);
            }
            parts.push('</tr>');
        }
    }

    parts.push('</tbody></table>');
    return parts.join("");
}

export const table = { render };
export default table;
