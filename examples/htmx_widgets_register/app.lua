-- htmx_widgets_register — exercises every widget in §1.5.g (8 widgets)
-- in one tiny CRUD app. Doubles as adoption reference AND the manual
-- UX-test surface (see README.md for the click-through checklist).

local db           = require("hull.db")
local form_parse   = require("hull.web.form")
local template     = require("hull.template")
local validate     = require("hull.validate")
local log          = require("hull.log")

-- HTMX core helpers + the 8 widgets
local htmx         = require("hull.web.htmx")
local toast        = require("hull.web.htmx.toast")
local confirm      = require("hull.web.htmx.confirm")
local form_widget  = require("hull.web.htmx.form")
local search       = require("hull.web.htmx.search")
local sort         = require("hull.web.htmx.sort")
local inline_edit  = require("hull.web.htmx.inline-edit")
local hxtable      = require("hull.web.htmx.table")
local hxpag        = require("hull.web.htmx.pagination")

local PER_PAGE = 8

app.manifest({
    modules = {
        "hull/http-server@1",
        "hull/db@1",
        "hull/template@1",
        "hull/validate@1",
        "hull/log@1",
        "hull/web/form@1",
        "hull/web/pagination@1",
        "hull/web/htmx@1",
        "hull/web/htmx/toast@1",
        "hull/web/htmx/confirm@1",
        "hull/web/htmx/form@1",
        "hull/web/htmx/search@1",
        "hull/web/htmx/sort@1",
        "hull/web/htmx/inline-edit@1",
        "hull/web/htmx/table@1",
        "hull/web/htmx/pagination@1",
    },
    csp = "htmx",
})

-- ── Column schema (drives table.render) ─────────────────────────────
local SCHEMA = {
    { name = "id",         label = "ID" },
    { name = "name",       label = "Name",     sortable = true, editable = true },
    { name = "category",   label = "Category", sortable = true, editable = true },
    { name = "status",     label = "Status",   sortable = true,
        -- Custom render: status as a styled badge. Output is spliced
        -- raw so the app owns the safety; we HTML-escape via the
        -- standard pattern.
        render = function(value, _row)
            local v = tostring(value or "")
            local escaped = (v:gsub("[&<>\"']", {
                ["&"]="&amp;",["<"]="&lt;",[">"]="&gt;",['"']="&quot;",["'"]="&#39;"
            }))
            return '<span class="badge badge-' .. escaped .. '">'
                .. escaped .. '</span>'
        end,
    },
}

local SORT_ALLOWED = { "id", "name", "category", "status" }

-- ── Pre-rendered widget attribute strings ─────────────────────────
-- Templates can't call functions in {{ }} expressions, so widgets
-- run server-side and the rendered strings flow into template data.
-- These don't depend on per-request state, so build at module load.

local SEARCH_INPUT_ATTRS = search.input_attrs({
    url      = "/search",
    target   = "#grid",
    name     = "q",
    delay_ms = 250,
})

local DELETE_CONFIRM_ATTRS = confirm.attrs(
    "Delete this asset? This cannot be undone.",
    { danger = true, yes = "Delete" })

-- ── Helpers ────────────────────────────────────────────────────────

-- LIKE-escape % _ \ so a query of "100%" matches the literal string.
local function like_pattern(q)
    return "%" .. q:gsub("[\\%%_]", "\\%0") .. "%"
end

local function trim(s) return (s or ""):gsub("^%s+", ""):gsub("%s+$", "") end

local function valid_id(s)
    local n = tonumber(s)
    if n and n >= 1 and n == math.floor(n) then return n end
    return nil
end

local function valid_col(s)
    for _, c in ipairs({ "name", "category" }) do
        if c == s then return c end
    end
    return nil
end

-- Build the {rows, current_sort, total} bundle every grid response needs.
local function load_page(req, q)
    local current = sort.parse(req, { allowed = SORT_ALLOWED, default = "id" })
    local page = tonumber(req.query.page) or 1
    if page < 1 then page = 1 end
    local offset = (page - 1) * PER_PAGE

    local total, rows
    if q == "" then
        total = db.query("SELECT COUNT(*) AS n FROM assets")[1].n
        rows = db.query(
            "SELECT id, name, category, status, created_at FROM assets "
            .. "ORDER BY " .. current.column .. " " .. current.direction
            .. " LIMIT ? OFFSET ?", { PER_PAGE, offset })
    else
        local p = like_pattern(q)
        total = db.query(
            "SELECT COUNT(*) AS n FROM assets WHERE name LIKE ? ESCAPE '\\' "
            .. "OR category LIKE ? ESCAPE '\\'", { p, p })[1].n
        rows = db.query(
            "SELECT id, name, category, status, created_at FROM assets "
            .. "WHERE name LIKE ? ESCAPE '\\' OR category LIKE ? ESCAPE '\\' "
            .. "ORDER BY " .. current.column .. " " .. current.direction
            .. " LIMIT ? OFFSET ?", { p, p, PER_PAGE, offset })
    end
    return { rows = rows, current_sort = current, total = total,
             page = page, q = q }
end

-- Pre-render the grid fragment (table + pagination nav). Same shape
-- whether the request came from htmx or full page; the wrapping
-- <div id="grid"> goes in the parent template.
local function render_grid(state)
    local base_url = "/search"
    if state.q ~= "" then
        base_url = base_url .. "?q=" .. state.q
        -- pagination + sort URLs need the &q to stay filtered
    end
    local table_html = hxtable.render(state.rows, SCHEMA, {
        base_url     = base_url,
        target       = "#grid",
        -- #grid is a container wrapping table + nav + actions;
        -- replace its contents, don't replace the container itself.
        swap         = "innerHTML",
        current_sort = state.current_sort,
        edit_url_for = function(row, col)
            return "/assets/" .. row.id .. "/" .. col .. "/edit"
        end,
        empty_label  = state.q ~= ""
            and ('No assets match "' .. state.q .. '".')
            or "No assets yet — add one below.",
    })
    local nav_html = hxpag.nav(state.total, {
        page = state.page, per_page = PER_PAGE,
        base_url = base_url, target = "#grid",
    })

    -- Per-row delete buttons live next to the table; emit them inline
    -- under each row. Simpler than threading per-cell custom render
    -- for an "actions" column — the README explains the trade-off.
    -- (For Phase 2 widget showcase, a row-level Actions column would
    -- be the next-tier add; keeping the example terse here.)
    local action_html = ''
    if #state.rows > 0 then
        local parts = { '<div class="action-bar">' }
        for _, r in ipairs(state.rows) do
            parts[#parts + 1] = ('<button class="del-btn" hx-delete="/assets/'
                .. r.id .. '" hx-target="#grid" hx-swap="innerHTML" '
                .. DELETE_CONFIRM_ATTRS
                .. ' title="Delete ' .. tostring(r.name) .. '">×&nbsp;'
                .. tostring(r.id) .. '</button>')
        end
        parts[#parts + 1] = '</div>'
        action_html = table.concat(parts)
    end
    return table_html .. nav_html .. action_html
end

-- ── Routes ─────────────────────────────────────────────────────────

app.get("/", function(req, res)
    local q = trim(req.query.q)
    local state = load_page(req, q)
    res:html(template.render("pages/home.html", {
        q                  = q,
        search_input_attrs = SEARCH_INPUT_ATTRS,
        grid_html          = render_grid(state),
        -- Form widget data (no errors on initial render).
        values             = {},
        name_attrs         = form_widget.field_attrs(nil, "name"),
        name_error         = form_widget.field_error(nil, "name"),
        category_attrs     = form_widget.field_attrs(nil, "category"),
        category_error     = form_widget.field_error(nil, "category"),
    }))
end)

-- Debounced search hits this. Returns just the #grid fragment.
app.get("/search", function(req, res)
    local q = trim(req.query.q)
    local state = load_page(req, q)
    res:html(render_grid(state))
end)

-- Add new asset (POST from the form widget).
app.post("/assets", function(req, res)
    local body = form_parse.parse(req.body or "")
    local ok, errors = validate.check(body, {
        name     = { required = true, trim = true, min = 1, max = 80,
                     message = "Name is required (1-80 chars)." },
        category = { required = true, trim = true, min = 1, max = 40,
                     message = "Category is required." },
    })
    if not ok then
        -- Re-render the form fragment with widget-rendered errors.
        htmx.retarget(res, "#new-form")
        htmx.reswap(res, "outerHTML")
        res:html(template.render("partials/_form.html", {
            values         = body,
            name_attrs     = form_widget.field_attrs(errors, "name"),
            name_error     = form_widget.field_error(errors, "name"),
            category_attrs = form_widget.field_attrs(errors, "category"),
            category_error = form_widget.field_error(errors, "category"),
        }))
        return
    end
    db.exec("INSERT INTO assets (name, category, status) VALUES (?, ?, 'active')",
        { body.name, body.category })

    -- Two responses on success: refresh the grid AND fire a toast.
    -- The toast helper sets HX-Trigger; htmx.compose isn't needed
    -- here because we're not also using HX-Trigger for anything else.
    toast.success(res, 'Added "' .. body.name .. '"')
    local state = load_page(req, "")
    htmx.retarget(res, "#grid")
    htmx.reswap(res, "innerHTML")
    res:html(render_grid(state))
end)

-- ── Inline-edit triad for any editable column ─────────────────────

local function _row(id)
    return db.query("SELECT * FROM assets WHERE id = ?", { id })[1]
end

-- Single edit handler for any column. The route's :col param is
-- validated against the editable-column allowlist.
app.get("/assets/:id/:col/edit", function(req, res)
    local id  = valid_id(req.params.id)
    local col = valid_col(req.params.col)
    if not id or not col then res:status(404); return end
    local r = _row(id); if not r then res:status(404); return end
    res:html(inline_edit.editor({
        value      = r[col],
        save_url   = "/assets/" .. id .. "/" .. col,
        cancel_url = "/assets/" .. id .. "/" .. col .. "/view",
        label      = "Edit " .. col,
    }))
end)

app.get("/assets/:id/:col/view", function(req, res)
    local id  = valid_id(req.params.id)
    local col = valid_col(req.params.col)
    if not id or not col then res:status(404); return end
    local r = _row(id); if not r then res:status(404); return end
    res:html(inline_edit.cell({
        value    = r[col],
        edit_url = "/assets/" .. id .. "/" .. col .. "/edit",
        label    = "Edit " .. col,
    }))
end)

app.patch("/assets/:id/:col", function(req, res)
    local id  = valid_id(req.params.id)
    local col = valid_col(req.params.col)
    if not id or not col then res:status(404); return end
    local body = form_parse.parse(req.body or "")
    local val  = trim(body.value)
    if val == "" then
        -- Re-render the editor with the same value so the user can fix.
        res:html(inline_edit.editor({
            value      = val,
            save_url   = "/assets/" .. id .. "/" .. col,
            cancel_url = "/assets/" .. id .. "/" .. col .. "/view",
            label      = "Edit " .. col .. " (cannot be empty)",
        }))
        return
    end
    db.exec("UPDATE assets SET " .. col .. " = ? WHERE id = ?",
        { val, id })
    toast.info(res, col .. " updated")
    res:html(inline_edit.cell({
        value    = val,
        edit_url = "/assets/" .. id .. "/" .. col .. "/edit",
        label    = "Edit " .. col,
    }))
end)

-- Delete + refresh grid + toast.
app.delete("/assets/:id", function(req, res)
    local id = valid_id(req.params.id)
    if not id then res:status(404); return end
    local r = _row(id)
    if not r then res:status(404); return end
    db.exec("DELETE FROM assets WHERE id = ?", { id })
    toast.success(res, 'Deleted "' .. r.name .. '"')
    local state = load_page(req, "")
    res:html(render_grid(state))
end)

log.info("htmx_widgets_register: loaded — visit / for the demo")
