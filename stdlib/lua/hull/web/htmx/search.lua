--- HTMX search widget — server-side helpers for a debounced
--- search input that posts to a server-rendered results partial.
--
-- @module hull.web.htmx.search
-- @license AGPL-3.0-or-later
--
-- This widget is htmx-native: no client JS is shipped. Apps add
-- one `<input>` with the attrs from `search.input_attrs(...)` and
-- a container with the attrs from `search.results_attrs(...)`;
-- the server renders fragments into the container on each
-- debounced request.
--
-- Setup:
--
--     app.manifest({
--         modules = {
--             "hull/web/htmx@1",
--             "hull/web/htmx/search@1",
--         },
--         csp = "htmx",
--     })
--
--     -- template:
--     <input placeholder="Search assets…"
--            {{ search.input_attrs({ url = "/assets/search" }) | raw }}>
--     <div {{ search.results_attrs() | raw }}></div>
--
--     -- handler:
--     app.get("/assets/search", function(req, res)
--         local q = req.query.q or ""
--         if #q < 2 then res:html("") return end  -- placeholder/skip
--         local rows = db.query("SELECT … WHERE name LIKE ?", { q .. "%" })
--         res:html(template.render("partials/asset_rows.html", { rows = rows }))
--     end)
--
-- Loading indicator: pair with htmx's standard `htmx-indicator`
-- convention. Add `hx-indicator="#spinner"` via `opts.indicator`,
-- and `<span id="spinner" class="htmx-indicator">…</span>` to the
-- markup. htmx ships the visibility CSS.

local search = {}

-- HTML attribute-value escaping. Same shape as the confirm widget;
-- duplicated here to avoid a cross-module dep on a tiny helper.
local ESC = {
    ["&"] = "&amp;",
    ['"'] = "&quot;",
    ["<"] = "&lt;",
    [">"] = "&gt;",
    ["'"] = "&#39;",
}
local function attr_escape(s)
    return (tostring(s):gsub('[&"<>\']', ESC))
end

--- Render the htmx attribute set for a debounced search input.
--
-- Required opts:
--   - `url`         string — server endpoint that returns the
--                            results partial.
--
-- Optional opts:
--   - `target`      string — CSS selector of the results container.
--                            Default: `"#hull-search-results"`
--                            (paired with `results_attrs()` default).
--   - `name`        string — query parameter name. Default: `"q"`.
--   - `delay_ms`    number — debounce window in ms. Default: 300.
--   - `method`      string — `"get"` (default) or `"post"`.
--                            POST is useful for long query strings
--                            or sensitive data; otherwise prefer
--                            GET (bookmarkable, cacheable).
--   - `push_url`    bool   — emit `hx-push-url="true"` so each
--                            search updates the browser URL bar.
--                            Default: false (typeahead UX rarely
--                            wants every keystroke in history).
--   - `indicator`   string — CSS selector for a loading spinner;
--                            emits `hx-indicator="<selector>"`.
--
-- Always also emits `type="search"`, `autocomplete="off"`, and a
-- `name="<q-name>"` so the server-rendered URL carries the query.
--
-- @tparam table opts
-- @treturn string  Attribute string for `| raw` splicing into <input>.
function search.input_attrs(opts)
    opts = opts or {}
    local url       = opts.url or ""
    local target    = opts.target or "#hull-search-results"
    local q_name    = opts.name or "q"
    local delay_ms  = opts.delay_ms or 300
    local method    = (opts.method == "post") and "post" or "get"
    local hx_attr   = "hx-" .. method
    local trigger   = "input changed delay:" .. tostring(delay_ms) .. "ms"

    local parts = {
        'type="search"',
        'name="' .. attr_escape(q_name) .. '"',
        'autocomplete="off"',
        hx_attr .. '="' .. attr_escape(url) .. '"',
        'hx-trigger="' .. attr_escape(trigger) .. '"',
        'hx-target="' .. attr_escape(target) .. '"',
    }
    if opts.push_url then
        parts[#parts + 1] = 'hx-push-url="true"'
    end
    if opts.indicator then
        parts[#parts + 1] = 'hx-indicator="' .. attr_escape(opts.indicator) .. '"'
    end
    return table.concat(parts, " ")
end

--- Render the a11y attribute set for the results container.
--
-- Optional opts:
--   - `id`  string — DOM id for the container. Default:
--                    `"hull-search-results"` (the default
--                    `input_attrs` target points here).
--
-- Always emits `aria-live="polite"` so assistive tech announces
-- result updates without interrupting current speech.
--
-- @tparam[opt] table opts
-- @treturn string  Attribute string for `| raw` splicing.
function search.results_attrs(opts)
    opts = opts or {}
    local id = opts.id or "hull-search-results"
    return 'id="' .. attr_escape(id) .. '" aria-live="polite"'
end

return search
