/**
 * @file hull:web:htmx:search
 * @module hull:web:htmx:search
 * @description HTMX search widget — server-side helpers for a
 * debounced search input that posts to a server-rendered results
 * partial.
 *
 * This widget is htmx-native: no client JS is shipped. Apps add
 * one `<input>` with the attrs from `search.inputAttrs(...)` and
 * a container with the attrs from `search.resultsAttrs(...)`;
 * the server renders fragments into the container on each
 * debounced request.
 *
 * Lua parity: same surface as `hull.web.htmx.search`. Names map
 * snake_case (Lua) → camelCase (JS).
 *
 *     // template:
 *     <input placeholder="Search assets…"
 *            {{{ search.inputAttrs({ url: "/assets/search" }) }}}>
 *     <div {{{ search.resultsAttrs() }}}></div>
 *
 *     // handler:
 *     app.get("/assets/search", (req, res) => {
 *         const q = req.query.q || "";
 *         if (q.length < 2) { res.html(""); return; }
 *         const rows = db.query("SELECT … WHERE name LIKE ?", [q + "%"]);
 *         res.html(template.render("partials/asset_rows.html", { rows }));
 *     });
 */

const ESC = { "&": "&amp;", '"': "&quot;", "<": "&lt;", ">": "&gt;", "'": "&#39;" };
function attrEscape(s) {
    return String(s == null ? "" : s).replace(/[&"<>']/g, (c) => ESC[c]);
}

/**
 * Render the htmx attribute set for a debounced search input.
 *
 * Required: opts.url. Optional: target (default
 * "#hull-search-results"), name (default "q"), delayMs (default
 * 300), method ("get" | "post", default "get"), pushUrl (default
 * false), indicator (CSS selector for hx-indicator).
 *
 * Always also emits type="search", name=..., autocomplete="off".
 *
 * @param {object} opts
 * @returns {string}  Attribute string for unescaped splice.
 */
function inputAttrs(opts) {
    opts = opts || {};
    const url      = opts.url || "";
    const target   = opts.target  || "#hull-search-results";
    const qName    = opts.name    || "q";
    const delayMs  = opts.delayMs || 300;
    const method   = opts.method === "post" ? "post" : "get";
    const hxAttr   = "hx-" + method;
    const trigger  = `input changed delay:${delayMs}ms`;

    const parts = [
        `type="search"`,
        `name="${attrEscape(qName)}"`,
        `autocomplete="off"`,
        `${hxAttr}="${attrEscape(url)}"`,
        `hx-trigger="${attrEscape(trigger)}"`,
        `hx-target="${attrEscape(target)}"`,
    ];
    if (opts.pushUrl)   parts.push(`hx-push-url="true"`);
    if (opts.indicator) parts.push(`hx-indicator="${attrEscape(opts.indicator)}"`);
    return parts.join(" ");
}

/**
 * Render the a11y attribute set for the results container.
 * Defaults to id="hull-search-results" (the inputAttrs default
 * target). Always emits aria-live="polite".
 */
function resultsAttrs(opts) {
    opts = opts || {};
    const id = opts.id || "hull-search-results";
    return `id="${attrEscape(id)}" aria-live="polite"`;
}

export const search = { inputAttrs, resultsAttrs };
export default search;
