--- Server-side offset-based pagination helper.
--
-- @module hull.web.pagination
-- @license AGPL-3.0-or-later
--
-- Reads `?page=N&per_page=M` from the request, builds SQL-ready
-- `offset` + `limit`, and renders a navigation structure suitable
-- for templates or JSON serialization. Useful for REST endpoints,
-- server-rendered pages, and HTMX fragment swaps alike.
--
-- This is the runtime-agnostic core: nothing about it knows HTMX.
-- The `partials/_pagination.html` template (shipped per-app, not
-- here) renders plain HTML anchors that work without JS; HTMX apps
-- add `hx-get` + `hx-target` on the anchors so clicks become
-- fragment swaps.
--
-- Cursor-based pagination is out of scope; use this for any list
-- where total-count + offset is acceptable.

local pagination = {}

local function clamp(n, lo, hi)
    if n < lo then return lo end
    if n > hi then return hi end
    return n
end

local function to_int(x, default)
    local n = tonumber(x)
    if not n or n ~= math.floor(n) or n < 0 then return default end
    return n
end

-- Build the URL for a given page, preserving (and not duplicating)
-- the per_page param when it differs from the default.
--
-- Contract for `base`: a path + optional query string (no fragment,
-- no CRLF). Reject CRLF/NUL hard — if a caller fed user input here,
-- a malformed base could land in a Location/Link header and become
-- a response-splitting vector. The function does NOT dedupe an
-- existing `page=` / `per_page=` in the base query string; callers
-- that pass a query-bearing base must own that themselves (cheaper
-- than re-parsing the query for every link in the nav).
local function url_for(base, page, per_page, default_per_page)
    if base and base:find("[\r\n%z]") then
        error("pagination: base_url must not contain CR / LF / NUL", 2)
    end
    if not base or base == "" then
        local url = "?page=" .. page
        if per_page and per_page ~= default_per_page then
            url = url .. "&per_page=" .. per_page
        end
        return url
    end
    local sep = base:find("?", 1, true) and "&" or "?"
    local url = base .. sep .. "page=" .. page
    if per_page and per_page ~= default_per_page then
        url = url .. "&per_page=" .. per_page
    end
    return url
end

--- Extract pagination params from `req.query`.
--
-- Reads `req.query.page` (1-indexed, default 1) and
-- `req.query.per_page` (default 20, capped at 100). Invalid values
-- (non-numeric, negative, fractional) fall back to defaults.
--
-- @tparam table req                  Current request.
-- @tparam[opt] table opts
--   @tparam[opt=20] integer opts.default_per_page
--   @tparam[opt=100] integer opts.max_per_page
-- @treturn table  `{ page, per_page, offset, limit }`. `offset` and
--   `limit` are SQL-ready: pass straight to `LIMIT ? OFFSET ?`.
--
-- @usage
--   local p = pagination.from_query(req)
--   local rows = db.query("SELECT * FROM todos ORDER BY id "
--                         .. "LIMIT ? OFFSET ?", { p.limit, p.offset })
function pagination.from_query(req, opts)
    opts = opts or {}
    local default_per_page = opts.default_per_page or 20
    local max_per_page     = opts.max_per_page or 100
    local query = (req and req.query) or {}
    -- Upper bound on page is 1e9. Without it, `?page=9007199254740992`
    -- (anywhere near 2^53) flows into `offset = (page-1) * per_page`
    -- producing trillion-row SQL offsets — SQLite handles them in
    -- O(1) before scanning, but the math is wasted and the bound
    -- should be small enough to fit in any sane integer type.
    local page = clamp(to_int(query.page, 1), 1, 1e9)
    local per_page = clamp(to_int(query.per_page, default_per_page),
                           1, max_per_page)
    return {
        page = page,
        per_page = per_page,
        offset = (page - 1) * per_page,
        limit = per_page,
    }
end

--- Build a pagination-navigation structure from a total count.
--
-- Returns a table suitable for template rendering or JSON serialization.
-- Links use a windowed pattern: `[1] ... [page-W .. page+W] ... [last]`
-- with ellipses between non-adjacent runs. Single-page results
-- collapse `show` to `false`.
--
-- @tparam integer total  Total row count (e.g. from `SELECT COUNT(*)`).
-- @tparam table opts
--   @tparam integer  opts.page              Current page (1-indexed).
--   @tparam integer  opts.per_page          Rows per page.
--   @tparam[opt] integer opts.default_per_page  Same as from_query default;
--     used to decide whether to include `per_page=` in URLs.
--   @tparam[opt=""] string opts.base_url    URL the links point at
--     (e.g. `"/todos"`). Existing `?` or `&` are detected.
--   @tparam[opt=2] integer opts.window      Pages either side of current
--     to always include.
-- @treturn table  `{ total, pages, page, per_page, prev, next,
--                    has_prev, has_next, show, links }` where links is
--   an array of `{ page, url, active }` or `{ ellipsis = true }` entries.
function pagination.render(total, opts)
    opts = opts or {}
    local per_page = opts.per_page or 20
    local default_per_page = opts.default_per_page or per_page
    local base = opts.base_url or ""
    local window = opts.window or 2
    if window < 0 then window = 0 end
    if total < 0 then total = 0 end

    local pages = math.max(1, math.ceil(total / per_page))
    local page = clamp(opts.page or 1, 1, pages)

    -- Collect target page numbers as a set, then sort. Using a set
    -- naturally dedupes when the window overlaps the boundaries.
    local targets = { [1] = true, [pages] = true, [page] = true }
    for n = page - window, page + window do
        if n >= 1 and n <= pages then targets[n] = true end
    end
    local nums = {}
    for n in pairs(targets) do nums[#nums + 1] = n end
    table.sort(nums)

    -- Build links, inserting an ellipsis marker between non-adjacent
    -- runs. Adjacent (n - prev == 1) means no gap.
    local links = {}
    local prev_n = nil
    for _, n in ipairs(nums) do
        if prev_n and n - prev_n > 1 then
            links[#links + 1] = { ellipsis = true }
        end
        links[#links + 1] = {
            page = n,
            url = url_for(base, n, per_page, default_per_page),
            active = (n == page),
        }
        prev_n = n
    end

    local has_prev = page > 1
    local has_next = page < pages
    return {
        total    = total,
        pages    = pages,
        page     = page,
        per_page = per_page,
        prev     = has_prev and url_for(base, page - 1, per_page, default_per_page) or nil,
        next     = has_next and url_for(base, page + 1, per_page, default_per_page) or nil,
        has_prev = has_prev,
        has_next = has_next,
        show     = pages > 1,
        links    = links,
    }
end

return pagination
