--- HTMX sort widget — server-side helpers for sortable column
--- headers driven by `?sort=col[:asc|desc]`.
--
-- @module hull.web.htmx.sort
-- @license AGPL-3.0-or-later
--
-- Two pure-function helpers:
--
--   sort.parse(req, opts?)     -- read + validate ?sort param
--   sort.header_attrs(col, current, opts)
--                              -- htmx attrs + indicator class for a <th>
--
-- App owns the table markup; this widget owns the URL convention
-- and the toggle/indicator logic. Pairs naturally with
-- hull/web/htmx/table@1 which calls header_attrs for each
-- sortable column.
--
-- URL convention:
--   ?sort=name          -- ascending (default direction)
--   ?sort=name:asc      -- explicit ascending
--   ?sort=name:desc     -- descending
--   ?sort=              -- missing → fall back to opts.default or nil
--
-- Toggle semantics (what the link clicks emit):
--   - first click on an unsorted column → asc
--   - click on already-asc column       → desc
--   - click on already-desc column      → asc (full cycle, not "none")
--
-- The "no sort" state isn't reachable via clicks; it's the initial
-- state and is selectable only by clearing the URL. This matches
-- the user expectation that clicking a column header always sorts
-- something (vs. a "third click" that surprisingly restores
-- whatever order the database happened to return).
--
-- Setup:
--
--     app.manifest({
--         modules = {
--             "hull/web/htmx@1",
--             "hull/web/htmx/sort@1",
--         },
--         csp = "htmx",
--     })
--
-- Usage (handler):
--
--     local sort = require("hull.web.htmx.sort")
--
--     app.get("/assets", function(req, res)
--         local current = sort.parse(req, {
--             allowed = { "name", "category", "status" },
--             default = "name",
--         })
--         local rows = db.query(
--             "SELECT * FROM assets ORDER BY " .. current.column
--             .. " " .. current.direction
--             -- column is allowlist-checked → safe to splice; direction
--             -- is constrained to "asc" / "desc" by sort.parse → safe.
--         )
--         res:html(template.render("pages/assets.html", {
--             rows = rows,
--             name_sort_attrs = sort.header_attrs("name", current, {
--                 url = "/assets", target = "#asset-table",
--             }),
--             -- ... per column
--         }))
--     end)
--
-- Template:
--     <th {{ name_sort_attrs | raw }}>Name</th>
--
-- The emitted attrs include:
--   - hx-get="<url>?sort=<col>[:dir]"  -- toggled URL
--   - hx-target="<target>"
--   - hx-push-url="true" (when opts.push_url is truthy)
--   - role="button" tabindex="0"  -- keyboard-activatable
--   - data-sort-direction="asc|desc|none"  -- styling hook
--   - class="hull-sort-header hull-sort-{asc|desc|none}"
--   - aria-sort="ascending|descending|none"  -- a11y standard

local sort = {}

-- HTML attribute-value escaping; same shape as the other widgets.
local ESC = {
    ["&"] = "&amp;",
    ['"'] = "&quot;",
    ["<"] = "&lt;",
    [">"] = "&gt;",
    ["'"] = "&#39;",
}
local function attr_escape(s)
    return (tostring(s == nil and "" or s):gsub('[&"<>\']', ESC))
end

-- Column-name safety: only [A-Za-z0-9_-] reaches the URL or the SQL
-- ORDER BY (after the app's allowlist check). Defense in depth
-- against URL-injection or accidental allowlist bypass.
local function safe_column(s)
    if s == nil then return "" end
    return (tostring(s):gsub("[^%w_-]", ""))
end

local function check_scalar(value, name)
    if value == nil then return end
    local t = type(value)
    if t ~= "string" and t ~= "number" then
        error("sort: " .. name .. " must be a string or number, got " .. t, 3)
    end
end

--- Parse `?sort=col[:asc|desc]` from the request.
--
-- @tparam table req     Keel request.
-- @tparam[opt] table opts
-- @tparam[opt] table opts.allowed  Array of column names that
--                                  callers permit. Anything else
--                                  is rejected (falls back to
--                                  opts.default or nil).
-- @tparam[opt] string opts.default Column to use when no valid
--                                  sort param is present.
-- @treturn ?table { column = string, direction = "asc"|"desc" }
--                 or nil if no allowed column was identified.
function sort.parse(req, opts)
    opts = opts or {}
    local raw = req and req.query and req.query.sort or nil
    local column, direction
    if raw and raw ~= "" then
        local c, d = raw:match("^([^:]+):(.+)$")
        if not c then
            c, d = raw, "asc"
        end
        column, direction = c, d
    end
    -- Normalize + allowlist-check.
    if column then
        column = safe_column(column)
        if column == "" then column = nil end
    end
    if opts.allowed and column then
        local found = false
        for _, name in ipairs(opts.allowed) do
            if name == column then found = true; break end
        end
        if not found then column = nil end
    end
    if not column and opts.default then
        -- Defense in depth: opts.default could be user-controlled
        -- (e.g. via app config or a per-route override). Run it
        -- through the same sanitize + allowlist pipeline so a
        -- mistake in the default value can't bypass the column
        -- safety story.
        local d = safe_column(opts.default)
        if d ~= "" then
            if not opts.allowed then
                column = d
            else
                for _, name in ipairs(opts.allowed) do
                    if name == d then column = d; break end
                end
            end
            if column then direction = direction or "asc" end
        end
    end
    if not column then return nil end
    direction = (direction == "desc") and "desc" or "asc"
    return { column = column, direction = direction }
end

-- Compute the URL fragment + direction-data for a column given the
-- current sort state. Returns { url_sort, next_direction, this_direction }.
local function plan_toggle(column, current)
    local this_direction = "none"
    local next_direction = "asc"
    if current and current.column == column then
        this_direction = current.direction
        next_direction = (current.direction == "asc") and "desc" or "asc"
    end
    -- URL emits :asc explicitly so the toggle round-trips
    -- (omitting it makes "asc" the implicit default, but explicit
    -- is friendlier for back-button shareability).
    local url_sort = column .. ":" .. next_direction
    return url_sort, next_direction, this_direction
end

-- aria-sort values match the WAI-ARIA spec.
local ARIA_SORT = { asc = "ascending", desc = "descending", none = "none" }

--- Render htmx attrs + classes for a single column header.
--
-- @tparam string column   This column's name (URL + data attrs).
-- @tparam ?table current  Output of sort.parse — what's currently sorted.
-- @tparam table opts
-- @tparam string  opts.url       Base URL (no query); the helper
--                                appends ?sort=col[:dir].
-- @tparam[opt] string opts.target   hx-target. Defaults to
--                                   "closest table" so the table
--                                   it wraps is swapped.
-- @tparam[opt] bool opts.push_url   Emit hx-push-url="true".
--                                   Default true (sorted URLs are
--                                   bookmarkable / back-button
--                                   friendly).
-- @treturn string  Attribute string for `| raw` splicing into <th>.
function sort.header_attrs(column, current, opts)
    check_scalar(column, "column")
    opts = opts or {}
    check_scalar(opts.url,    "opts.url")
    check_scalar(opts.target, "opts.target")
    local url        = opts.url or ""
    local target     = opts.target or "closest table"
    local push_url   = opts.push_url
    if push_url == nil then push_url = true end

    local url_sort, _, this_direction = plan_toggle(column, current)
    local sep        = url:find("?", 1, true) and "&" or "?"
    local full_url   = url .. sep .. "sort=" .. url_sort

    local parts = {
        'role="button"',
        'tabindex="0"',
        'class="hull-sort-header hull-sort-' .. this_direction .. '"',
        'data-sort-column="' .. attr_escape(column) .. '"',
        'data-sort-direction="' .. this_direction .. '"',
        'aria-sort="' .. ARIA_SORT[this_direction] .. '"',
        'hx-get="' .. attr_escape(full_url) .. '"',
        'hx-target="' .. attr_escape(target) .. '"',
        'hx-trigger="click, keyup[key==\'Enter\'] from:this, keyup[key==\' \'] from:this"',
        'hx-swap="outerHTML"',
    }
    if push_url then
        parts[#parts + 1] = 'hx-push-url="true"'
    end
    return table.concat(parts, " ")
end

return sort
