--- HTMX pagination widget - htmx-attributed nav over the
--- existing hull/web/pagination@1 page-math.
--
-- @module hull.web.htmx.pagination
-- @license AGPL-3.0-or-later
--
-- Eliminates the ~30-line `_pagination.html` partial every
-- hypermedia app writes by hand. Delegates page-math (page count,
-- prev/next bounds, ellipsis insertion) to hull/web/pagination
-- @1; renders the nav HTML with hx-get + hx-target + optional
-- hx-push-url attrs spliced into each link.
--
-- One helper:
--
--   pagination.nav(total, opts)  -- rendered <nav> HTML string
--
-- The existing hull/web/pagination@1 module is unchanged; callers
-- that want plain (non-htmx) page reloads keep using
-- `pagination.render(total, opts)` to get the structured table
-- and render their own markup.
--
-- Setup:
--
--     app.manifest({
--         modules = {
--             "hull/web/htmx@1",
--             "hull/web/htmx/pagination@1",
--             "hull/web/pagination@1",  -- transitive dep
--         },
--         csp = "htmx",
--     })
--
-- Usage (handler):
--
--     local hxpag = require("hull.web.htmx.pagination")
--
--     app.get("/", function(req, res)
--         local total = db.query("SELECT COUNT(*) AS n FROM assets")[1].n
--         local p = require("hull.web.pagination").from_query(req)
--         local nav = hxpag.nav(total, {
--             page             = p.page,
--             per_page         = p.per_page,
--             base_url         = "/",
--             target           = "#asset-feed",
--             push_url         = true,
--         })
--         res:html(template.render("pages/home.html", {
--             rows = ...,
--             pagination_nav = nav,
--         }))
--     end)
--
-- Template (replaces the entire 30-line _pagination.html):
--     {{ pagination_nav | raw }}

local base_pagination = require("hull.web.pagination")

local pagination = {}

-- HTML attribute-value escaping (shared across the htmx widgets).
local attr_escape = require("hull.web.htmx").escape

-- Render the attrs that go on every active page link. Same set on
-- prev / next / numbered pages so the htmx behavior is uniform.
local function link_attrs(url, target, push_url)
    local parts = {
        'hx-get="' .. attr_escape(url) .. '"',
        'hx-target="' .. attr_escape(target) .. '"',
        'hx-swap="innerHTML"',
        'href="' .. attr_escape(url) .. '"',  -- progressive enhancement
    }
    if push_url then
        parts[#parts + 1] = 'hx-push-url="true"'
    end
    return table.concat(parts, " ")
end

--- Render the htmx-attributed pagination nav.
--
-- @tparam integer total       Total record count (for page-math).
-- @tparam table opts
-- @tparam[opt] integer  opts.page             Current page (1-based).
-- @tparam[opt] integer  opts.per_page         Page size.
-- @tparam[opt] integer  opts.default_per_page Default page size for
--                                             clean URLs (URL omits
--                                             per_page when it equals
--                                             default).
-- @tparam[opt] string   opts.base_url         Base URL (no ?page).
-- @tparam[opt] integer  opts.window           Pages either side of
--                                             current to always show.
--                                             Default 2.
-- @tparam[opt] string   opts.target           hx-target for nav links.
--                                             Default "body" (forces
--                                             full re-render - callers
--                                             usually want a content
--                                             container).
-- @tparam[opt] bool     opts.push_url         Default true.
-- @treturn string  `<nav>...</nav>` or "" when pages <= 1.
function pagination.nav(total, opts)
    opts = opts or {}
    local target   = opts.target or "body"
    local push_url = opts.push_url
    if push_url == nil then push_url = true end

    -- Delegate page-math.
    local p = base_pagination.render(total, opts)
    if not p.show then return "" end

    local parts = {
        '<nav class="hull-pagination" aria-label="Pagination"><ul>',
    }

    if p.has_prev then
        parts[#parts + 1] = '<li><a ' .. link_attrs(p.prev, target, push_url)
            .. ' rel="prev" aria-label="Previous page">&larr;</a></li>'
    end

    for _, link in ipairs(p.links) do
        if link.ellipsis then
            parts[#parts + 1] = '<li aria-hidden="true">&hellip;</li>'
        elseif link.active then
            -- link.page is always an integer from base_pagination
            -- today, but escape defensively to match the rest of
            -- the tier (and survive any future change to the
            -- base module's contract).
            parts[#parts + 1] = '<li><a aria-current="page" class="hull-pagination-active">'
                .. attr_escape(link.page) .. '</a></li>'
        else
            parts[#parts + 1] = '<li><a ' .. link_attrs(link.url, target, push_url)
                .. '>' .. attr_escape(link.page) .. '</a></li>'
        end
    end

    if p.has_next then
        parts[#parts + 1] = '<li><a ' .. link_attrs(p.next, target, push_url)
            .. ' rel="next" aria-label="Next page">&rarr;</a></li>'
    end

    parts[#parts + 1] = '</ul></nav>'
    return table.concat(parts)
end

return pagination
