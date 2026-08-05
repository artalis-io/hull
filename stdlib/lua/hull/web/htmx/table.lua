--- HTMX table widget — schema-driven <table> renderer that
--- composes sort + inline-edit per column.
--
-- @module hull.web.htmx.table
-- @license AGPL-3.0-or-later
--
-- The grid pattern of every admin / data UI in one helper. Takes
-- rows from a DB query + a column schema; emits a complete
-- `<table><thead><tbody>` block with:
--
--   - Sortable column headers via hull/web/htmx/sort
--     (per column: sortable=true)
--   - Inline-editable cells via hull/web/htmx/inline-edit
--     (per column: editable=true; requires opts.edit_url_for)
--   - Plain text cells for everything else (HTML-escaped)
--
-- Apps wire the search input + pagination footer around the
-- table separately using hull/web/htmx/search and
-- hull/web/htmx/pagination — this widget owns the grid, the
-- header pattern, and the cell rendering, not the page layout.
--
-- One helper:
--
--   table.render(rows, schema, opts)
--
-- Setup:
--
--     app.manifest({
--         modules = {
--             "hull/web/htmx@1",
--             "hull/web/htmx/table@1",
--             -- table's transitive deps (sort, inline-edit, htmx)
--             -- are pulled in automatically by the resolver, but
--             -- nothing prevents declaring them explicitly.
--         },
--         csp = "htmx",
--     })
--
-- Usage:
--
--     local tbl   = require("hull.web.htmx.table")
--     local sort  = require("hull.web.htmx.sort")
--
--     local SCHEMA = {
--         { name = "id",       label = "ID" },
--         { name = "name",     label = "Name",     sortable = true, editable = true },
--         { name = "category", label = "Category", sortable = true },
--         { name = "status",   label = "Status",   sortable = true },
--     }
--
--     app.get("/assets", function(req, res)
--         local current = sort.parse(req, {
--             allowed = { "id", "name", "category", "status" },
--             default = "id",
--         })
--         local rows = db.query("SELECT * FROM assets ORDER BY "
--             .. current.column .. " " .. current.direction)
--         local html = tbl.render(rows, SCHEMA, {
--             base_url = "/assets",          -- for sort + inline-edit URLs
--             target   = "#asset-table",     -- where sort swaps land
--             current_sort = current,        -- for the direction arrows
--             edit_url_for = function(row, col)
--                 -- Per-row edit endpoint. The widget calls this for
--                 -- every cell whose schema entry has editable = true.
--                 return "/assets/" .. row.id .. "/" .. col .. "/edit"
--             end,
--         })
--         res:html(template.render("pages/assets.html", {
--             asset_table = html,
--         }))
--     end)
--
-- Template:
--     <div id="asset-table">{{ asset_table | raw }}</div>

local sort_widget = require("hull.web.htmx.sort")
local inline_edit = require("hull.web.htmx.inline-edit")

local M = {}

local ESC = {
    ["&"] = "&amp;",
    ['"'] = "&quot;",
    ["<"] = "&lt;",
    [">"] = "&gt;",
    ["'"] = "&#39;",
}
local function esc(s)
    return (tostring(s == nil and "" or s):gsub('[&"<>\']', ESC))
end

local function check_required_opt(opts, name)
    if not opts or opts[name] == nil then
        error("table.render: opts." .. name .. " is required", 3)
    end
end

--- Render a sortable, inline-editable `<table>` from rows + schema.
--
-- @tparam table rows    Array of row tables. Each row is read by
--                       column name from the schema (row[col.name]).
-- @tparam table schema  Array of column specs:
--                         { name = "...",     -- key in row + URL
--                           label = "...",    -- header text
--                           sortable = bool,  -- emit sort attrs
--                           editable = bool,  -- wrap cell in inline-edit
--                           render   = fn?,   -- (value, row) -> string;
--                                             -- overrides default HTML escape
--                         }
-- @tparam table opts
-- @tparam string opts.base_url            Required. Base for sort
--                                         and inline-edit URLs.
-- @tparam[opt] string opts.target         hx-target for sort swaps.
--                                         Default "closest table".
-- @tparam[opt] string opts.swap           hx-swap for sort swaps.
--                                         When opts.target wraps
--                                         table + nav + actions
--                                         (e.g. "#grid"), pass
--                                         "innerHTML". When opts.target
--                                         is the table itself
--                                         (the default "closest table"),
--                                         pass "outerHTML". Omitted
--                                         by default — htmx then
--                                         uses its own default,
--                                         "innerHTML".
-- @tparam[opt] ?table opts.current_sort   Output of sort.parse —
--                                         drives the direction
--                                         arrows / aria-sort on
--                                         sortable headers.
-- @tparam[opt] bool opts.push_url         For sort headers. Default true.
-- @tparam[opt] function opts.edit_url_for (row, col_name) -> string
--                                         Required if any schema
--                                         entry has editable = true.
-- @tparam[opt] string opts.empty_label    Text for "no rows" state.
--                                         Default "No rows.".
-- @treturn string  Full `<table>...</table>` HTML.
function M.render(rows, schema, opts)
    opts = opts or {}
    check_required_opt(opts, "base_url")
    rows   = rows or {}
    schema = schema or {}

    local parts = { '<table class="hull-table"><thead><tr>' }

    -- Header row.
    for _, col in ipairs(schema) do
        if col.sortable then
            local attrs = sort_widget.header_attrs(col.name,
                opts.current_sort,
                {
                    url      = opts.base_url,
                    target   = opts.target,
                    swap     = opts.swap,
                    push_url = opts.push_url,
                })
            parts[#parts + 1] = '<th ' .. attrs .. '>'
                .. esc(col.label or col.name) .. '</th>'
        else
            parts[#parts + 1] = '<th>' .. esc(col.label or col.name) .. '</th>'
        end
    end
    parts[#parts + 1] = '</tr></thead><tbody>'

    -- Body rows.
    if #rows == 0 then
        -- #schema is always a non-negative integer (Lua # on an
        -- array); escape kept for consistency with the rest of
        -- the tier's body-escape discipline.
        parts[#parts + 1] = '<tr class="hull-table-empty"><td colspan="'
            .. esc(#schema) .. '">'
            .. esc(opts.empty_label or "No rows.")
            .. '</td></tr>'
    else
        for _, row in ipairs(rows) do
            parts[#parts + 1] = '<tr>'
            for _, col in ipairs(schema) do
                local value = row[col.name]
                local cell_html
                if col.render then
                    -- CONTRACT: a custom render owns its own escaping; its
                    -- return is spliced into <td> RAW (see docs/htmx_widgets.md).
                    -- If a column shows user-controlled data, the render fn MUST
                    -- escape it (the shipped example does). Defensive net: a
                    -- non-string return (a bare number/bool from a naive
                    -- `render=function(v) return v end`) is routed through esc
                    -- so it can't inject and can't raise a concat error.
                    cell_html = col.render(value, row)
                    if type(cell_html) ~= "string" then
                        cell_html = esc(cell_html)
                    end
                elseif col.editable then
                    if not opts.edit_url_for then
                        error("table.render: opts.edit_url_for is required when "
                            .. "any column has editable = true", 2)
                    end
                    cell_html = inline_edit.cell({
                        value    = value,
                        edit_url = opts.edit_url_for(row, col.name),
                        label    = "Edit " .. (col.label or col.name),
                    })
                else
                    cell_html = esc(value)
                end
                parts[#parts + 1] = '<td>' .. cell_html .. '</td>'
            end
            parts[#parts + 1] = '</tr>'
        end
    end

    parts[#parts + 1] = '</tbody></table>'
    return table.concat(parts)
end

return M
