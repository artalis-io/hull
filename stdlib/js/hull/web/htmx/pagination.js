/**
 * @file hull:web:htmx:pagination
 * @module hull:web:htmx:pagination
 * @description HTMX pagination widget — htmx-attributed nav over
 * the existing hull:web:pagination page-math.
 *
 * Eliminates the ~30-line pagination partial every hypermedia
 * app writes by hand. Delegates page-math to hull:web:pagination;
 * renders nav HTML with hx-get + hx-target + optional hx-push-url
 * attrs spliced into each link.
 *
 * One helper:
 *
 *   pagination.nav(total, opts)  -- rendered <nav> HTML string
 *
 * Lua parity: same surface as hull.web.htmx.pagination. Names map
 * snake_case → camelCase.
 */

import { pagination as basePagination } from "hull:web:pagination";

const ESC = { "&": "&amp;", '"': "&quot;", "<": "&lt;", ">": "&gt;", "'": "&#39;" };
function attrEscape(s) {
    return String(s == null ? "" : s).replace(/[&"<>']/g, (c) => ESC[c]);
}

function linkAttrs(url, target, pushUrl) {
    const parts = [
        `hx-get="${attrEscape(url)}"`,
        `hx-target="${attrEscape(target)}"`,
        `hx-swap="innerHTML"`,
        `href="${attrEscape(url)}"`,  // progressive enhancement
    ];
    if (pushUrl) parts.push(`hx-push-url="true"`);
    return parts.join(" ");
}

/**
 * Render the htmx-attributed pagination nav.
 *
 * @param {number} total      Total record count (for page-math).
 * @param {object} opts
 * @param {number}  [opts.page]
 * @param {number}  [opts.perPage]
 * @param {number}  [opts.defaultPerPage]
 * @param {string}  [opts.baseUrl]
 * @param {number}  [opts.window]    Default 2.
 * @param {string}  [opts.target]    hx-target. Default "body".
 * @param {boolean} [opts.pushUrl]   Default true.
 * @returns {string}  `<nav>...</nav>` or "" when pages <= 1.
 */
function nav(total, opts) {
    opts = opts || {};
    const target  = opts.target || "body";
    const pushUrl = opts.pushUrl !== false;

    // Delegate page-math. The base render() consumes snake_case
    // opts (per_page, base_url, default_per_page) — map them in.
    const p = basePagination.render(total, {
        page:             opts.page,
        per_page:         opts.perPage,
        default_per_page: opts.defaultPerPage,
        base_url:         opts.baseUrl,
        window:           opts.window,
    });
    if (!p.show) return "";

    const parts = ['<nav class="hull-pagination" aria-label="Pagination"><ul>'];

    if (p.has_prev) {
        parts.push(`<li><a ${linkAttrs(p.prev, target, pushUrl)} rel="prev" aria-label="Previous page">&larr;</a></li>`);
    }
    for (const link of p.links) {
        if (link.ellipsis) {
            parts.push('<li aria-hidden="true">&hellip;</li>');
        } else if (link.active) {
            // link.page is an integer from basePagination today;
            // escape defensively to match the rest of the tier.
            parts.push(`<li><a aria-current="page" class="hull-pagination-active">${attrEscape(link.page)}</a></li>`);
        } else {
            parts.push(`<li><a ${linkAttrs(link.url, target, pushUrl)}>${attrEscape(link.page)}</a></li>`);
        }
    }
    if (p.has_next) {
        parts.push(`<li><a ${linkAttrs(p.next, target, pushUrl)} rel="next" aria-label="Next page">&rarr;</a></li>`);
    }
    parts.push('</ul></nav>');
    return parts.join("");
}

export const pagination = { nav };
export default pagination;
