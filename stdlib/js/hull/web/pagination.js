/**
 * @file hull:web:pagination
 * @module hull:web:pagination
 * @description Server-side offset-based pagination helper. Lua
 *   parity: `hull.web.pagination`.
 *
 * Reads `?page=N&per_page=M` from the request, builds SQL-ready
 * `offset` + `limit`, and renders a navigation structure suitable
 * for templates or JSON serialization. Useful for REST endpoints,
 * server-rendered pages, and HTMX fragment swaps alike.
 *
 * This is the runtime-agnostic core: nothing about it knows HTMX.
 * The `partials/_pagination.html` template (shipped per-app, not
 * here) renders plain HTML anchors that work without JS; HTMX apps
 * add `hx-get` + `hx-target` on the anchors so clicks become
 * fragment swaps.
 *
 * Cursor-based pagination is out of scope; use this for any list
 * where total-count + offset is acceptable.
 *
 * @license AGPL-3.0-or-later
 */

function clamp(n, lo, hi) {
    if (n < lo) return lo;
    if (n > hi) return hi;
    return n;
}

function toInt(x, defaultVal) {
    const n = Number(x);
    if (!Number.isInteger(n) || n < 0) return defaultVal;
    return n;
}

// Build the URL for a given page, preserving (and not duplicating)
// the per_page param when it differs from the default.
function urlFor(base, page, perPage, defaultPerPage) {
    if (!base) {
        let url = "?page=" + page;
        if (perPage && perPage !== defaultPerPage) {
            url += "&per_page=" + perPage;
        }
        return url;
    }
    const sep = base.includes("?") ? "&" : "?";
    let url = base + sep + "page=" + page;
    if (perPage && perPage !== defaultPerPage) {
        url += "&per_page=" + perPage;
    }
    return url;
}

/**
 * Extract pagination params from `req.query`.
 *
 * Reads `req.query.page` (1-indexed, default 1) and
 * `req.query.per_page` (default 20, capped at 100). Invalid values
 * (non-numeric, negative, fractional) fall back to defaults.
 *
 * @param {object} req
 * @param {object} [opts]
 * @param {number} [opts.defaultPerPage=20]
 * @param {number} [opts.maxPerPage=100]
 * @returns {{page:number, per_page:number, offset:number, limit:number}}
 *   `offset` and `limit` are SQL-ready: pass straight to
 *   `LIMIT ? OFFSET ?`.
 *
 * @example
 *   const p = pagination.fromQuery(req);
 *   const rows = db.query(
 *       "SELECT * FROM todos ORDER BY id LIMIT ? OFFSET ?",
 *       [p.limit, p.offset]
 *   );
 */
function fromQuery(req, opts) {
    opts = opts || {};
    const defaultPerPage = opts.defaultPerPage || 20;
    const maxPerPage     = opts.maxPerPage || 100;
    const query = (req && req.query) || {};
    const page = clamp(toInt(query.page, 1), 1, Number.MAX_SAFE_INTEGER);
    const perPage = clamp(toInt(query.per_page, defaultPerPage),
                          1, maxPerPage);
    return {
        page,
        per_page: perPage,
        offset: (page - 1) * perPage,
        limit: perPage,
    };
}

/**
 * Build a pagination-navigation structure from a total count.
 *
 * Returns an object suitable for template rendering or JSON
 * serialization. Links use a windowed pattern:
 * `[1] ... [page-W .. page+W] ... [last]` with ellipses between
 * non-adjacent runs. Single-page results collapse `show` to `false`.
 *
 * @param {number} total  Total row count.
 * @param {object} opts
 * @param {number} opts.page              Current page (1-indexed).
 * @param {number} opts.per_page          Rows per page.
 * @param {number} [opts.defaultPerPage]  Same as fromQuery default;
 *   used to decide whether to include `per_page=` in URLs.
 * @param {string} [opts.baseUrl=""]      URL the links point at.
 * @param {number} [opts.window=2]        Pages either side of current.
 * @returns {object}  `{ total, pages, page, per_page, prev, next,
 *                       has_prev, has_next, show, links }`.
 *   `links` is an array of `{ page, url, active }` or
 *   `{ ellipsis: true }` entries.
 */
function render(total, opts) {
    opts = opts || {};
    const perPage = opts.per_page || 20;
    const defaultPerPage = opts.defaultPerPage || perPage;
    const base = opts.baseUrl || "";
    let window = opts.window == null ? 2 : opts.window;
    if (window < 0) window = 0;
    if (total < 0) total = 0;

    const pages = Math.max(1, Math.ceil(total / perPage));
    const page = clamp(opts.page || 1, 1, pages);

    // Collect target page numbers as a set, then sort numerically.
    const targets = new Set([1, pages, page]);
    for (let n = page - window; n <= page + window; n++) {
        if (n >= 1 && n <= pages) targets.add(n);
    }
    const nums = [...targets].sort((a, b) => a - b);

    // Build links, inserting an ellipsis between non-adjacent runs.
    const links = [];
    let prevN = null;
    for (const n of nums) {
        if (prevN != null && n - prevN > 1) {
            links.push({ ellipsis: true });
        }
        links.push({
            page: n,
            url: urlFor(base, n, perPage, defaultPerPage),
            active: (n === page),
        });
        prevN = n;
    }

    const hasPrev = page > 1;
    const hasNext = page < pages;
    return {
        total,
        pages,
        page,
        per_page: perPage,
        prev:     hasPrev ? urlFor(base, page - 1, perPage, defaultPerPage) : null,
        next:     hasNext ? urlFor(base, page + 1, perPage, defaultPerPage) : null,
        has_prev: hasPrev,
        has_next: hasNext,
        show:     pages > 1,
        links,
    };
}

const pagination = { fromQuery, render };
export { pagination };
