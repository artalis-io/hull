/**
 * @file hull:web:htmx:sort
 * @module hull:web:htmx:sort
 * @description HTMX sort widget — server-side helpers for
 * sortable column headers driven by `?sort=col[:asc|desc]`.
 *
 * Two pure-function helpers:
 *
 *   sort.parse(req, opts?)     -- read + validate ?sort param
 *   sort.headerAttrs(col, current, opts)
 *                              -- htmx attrs + indicator class for <th>
 *
 * URL convention: ?sort=name (asc default), ?sort=name:asc,
 * ?sort=name:desc. Toggle cycles asc → desc → asc (no "none"
 * state reachable via clicks; matches user expectation).
 *
 * Lua parity: same surface as `hull.web.htmx.sort`. Names map
 * snake_case → camelCase. URL emits :asc explicitly so toggled
 * URLs round-trip and stay bookmarkable.
 */

// HTML attribute-value escaping (shared across the htmx widgets).
import { htmx } from "hull:web:htmx";
const attrEscape = htmx.escape;
function safeColumn(s) {
    if (s == null) return "";
    return String(s).replace(/[^A-Za-z0-9_-]/g, "");
}
function checkScalar(value, name) {
    if (value == null) return;
    const t = typeof value;
    if (t !== "string" && t !== "number") {
        throw new Error(`sort: ${name} must be a string or number, got ${t}`);
    }
}

/**
 * Parse `?sort=col[:asc|desc]` from the request.
 * @param {object} req
 * @param {object} [opts]
 * @param {string[]} [opts.allowed]  Column-name whitelist; anything
 *                                   else falls back to opts.default.
 * @param {string}   [opts.default]  Default column when no valid
 *                                   sort param is present.
 * @returns {?{column: string, direction: "asc"|"desc"}}
 */
function parse(req, opts) {
    opts = opts || {};
    const raw = req && req.query && req.query.sort;
    let column = null;
    let direction = null;
    if (raw) {
        const m = String(raw).match(/^([^:]+):(.+)$/);
        if (m) { column = m[1]; direction = m[2]; }
        else   { column = String(raw); direction = "asc"; }
    }
    if (column) {
        column = safeColumn(column);
        if (column === "") column = null;
    }
    if (opts.allowed && column) {
        if (opts.allowed.indexOf(column) < 0) column = null;
    }
    if (!column && opts.default) {
        // Defense in depth: opts.default could be user-controlled
        // via app config. Run through the same sanitize + allowlist
        // pipeline so a mistake in the default can't bypass column
        // safety. Lua parity: stdlib/lua/hull/web/htmx/sort.lua.
        const d = safeColumn(opts.default);
        if (d) {
            if (!opts.allowed || opts.allowed.indexOf(d) >= 0) {
                column = d;
                direction = direction || "asc";
            }
        }
    }
    if (!column) return null;
    direction = direction === "desc" ? "desc" : "asc";
    return { column, direction };
}

const ARIA_SORT = { asc: "ascending", desc: "descending", none: "none" };

function planToggle(column, current) {
    let thisDirection = "none";
    let nextDirection = "asc";
    if (current && current.column === column) {
        thisDirection = current.direction;
        nextDirection = current.direction === "asc" ? "desc" : "asc";
    }
    return [column + ":" + nextDirection, nextDirection, thisDirection];
}

/**
 * Render htmx attrs + classes for a single column header.
 * @param {string} column
 * @param {?{column,direction}} current  Output of parse()
 * @param {object} opts
 * @param {string} opts.url       Base URL (no query string).
 * @param {string} [opts.target]  Default "closest table".
 * @param {string} [opts.swap]    hx-swap. When the target wraps
 *                                table + nav + actions (`#grid`),
 *                                pass "innerHTML". When the target
 *                                is the table itself, pass
 *                                "outerHTML". Omitted by default —
 *                                htmx falls back to "innerHTML".
 * @param {boolean} [opts.pushUrl] Default true.
 * @returns {string}  Attribute string for unescaped splice into <th>.
 */
function headerAttrs(column, current, opts) {
    checkScalar(column, "column");
    opts = opts || {};
    checkScalar(opts.url,    "opts.url");
    checkScalar(opts.target, "opts.target");
    checkScalar(opts.swap,   "opts.swap");
    const url      = opts.url || "";
    const target   = opts.target || "closest table";
    const pushUrl  = opts.pushUrl !== false;

    const [urlSort, , thisDirection] = planToggle(column, current);
    const sep      = url.indexOf("?") >= 0 ? "&" : "?";
    const fullUrl  = url + sep + "sort=" + urlSort;

    const parts = [
        // NO role="button". <th> has the implicit `columnheader`
        // ARIA role, and `aria-sort` is only valid on
        // `columnheader` / `rowheader` / `gridcell`. Overriding
        // with role="button" makes aria-sort an invalid combo
        // (axe.aria-allowed-attr) AND screen readers say "button"
        // instead of the more useful "column header, sorted
        // ascending". Native <th> + tabindex + sort.js keyboard
        // handler covers the click-by-key story.
        'tabindex="0"',
        `class="hull-sort-header hull-sort-${thisDirection}"`,
        `data-sort-column="${attrEscape(column)}"`,
        `data-sort-direction="${thisDirection}"`,
        `aria-sort="${ARIA_SORT[thisDirection]}"`,
        `hx-get="${attrEscape(fullUrl)}"`,
        `hx-target="${attrEscape(target)}"`,
        // hx-trigger is just `click`; the keyboard half (Enter +
        // Space) lives in /static/hull/htmx/sort/sort.js so it
        // works under csp = "htmx" (the [expr] filter syntax needs
        // Function() eval, which the strict preset rejects).
        'hx-trigger="click"',
    ];
    if (opts.swap) parts.push(`hx-swap="${attrEscape(opts.swap)}"`);
    if (pushUrl) parts.push('hx-push-url="true"');
    return parts.join(" ");
}

export const sort = { parse, headerAttrs };
export default sort;
