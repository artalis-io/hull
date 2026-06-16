--- HTMX inline-edit widget — click-to-edit single field, pure
--- hypermedia (no client JS for the mode switch).
--
-- @module hull.web.htmx.inline-edit
-- @license AGPL-3.0-or-later
--
-- Canonical htmx click-to-edit pattern. The cell starts as a
-- read-only `<span>` that issues `GET <edit_url>` on click; the
-- server returns the edit-form fragment. The form submits via
-- `PATCH <save_url>` and the server returns the display fragment.
-- Cancel re-fetches the display fragment via `GET <cancel_url>`,
-- which is typically the same as edit_url with mode=display (or
-- the parent record's URL — app-controlled).
--
-- Two server endpoints per editable field:
--
--   GET   edit_url    → return inline_edit.editor(...)
--   PATCH save_url    → validate, persist, return inline_edit.cell(...)
--   GET   cancel_url  → return inline_edit.cell(...)
--
-- A tiny client JS (~15 LOC at /static/hull/htmx/inline-edit/
-- inline-edit.js) handles the one thing pure htmx can't: focusing
-- the input after the edit fragment is swapped in. Esc-to-cancel
-- is handled via `hx-trigger` on the Cancel button — no JS.
--
-- Setup:
--
--     app.manifest({
--         modules = {
--             "hull/web/htmx@1",
--             "hull/web/htmx/inline-edit@1",
--         },
--         csp = "htmx",
--     })
--
--     -- base.html:
--     <link rel="stylesheet" href="/static/hull/htmx/inline-edit/inline-edit.css">
--     <script src="/static/hull/htmx/inline-edit/inline-edit.js" defer></script>
--
--     -- Initial render of an editable field. Hull's template
--     -- engine has no function-call support, so call inline_edit
--     -- helpers in the HANDLER and pass the pre-rendered string
--     -- as template data:
--     --
--     --   app.get("/assets/:id", function(req, res)
--     --       local asset = db.query("SELECT * FROM assets WHERE id=?",
--     --                              { req.params.id })[1]
--     --       res:html(template.render("pages/asset.html", {
--     --           asset = asset,
--     --           name_cell = inline_edit.cell({
--     --               value    = asset.name,
--     --               edit_url = "/assets/" .. asset.id .. "/name/edit",
--     --           }),
--     --       }))
--     --   end)
--     --
--     -- Template (pages/asset.html):
--     --   Name: {{ name_cell | raw }}
--
--     -- Edit endpoint:
--     app.get("/assets/:id/name/edit", function(req, res)
--         local asset = db.query("SELECT name FROM assets WHERE id=?",
--                                { req.params.id })[1]
--         res:html(inline_edit.editor({
--             value      = asset.name,
--             save_url   = "/assets/" .. req.params.id .. "/name",
--             cancel_url = "/assets/" .. req.params.id .. "/name/view",
--             label      = "Name",
--         }))
--     end)
--
--     -- Save endpoint:
--     app.patch("/assets/:id/name", function(req, res)
--         db.exec("UPDATE assets SET name=? WHERE id=?",
--                 { req.body.value, req.params.id })
--         res:html(inline_edit.cell({
--             value    = req.body.value,
--             edit_url = "/assets/" .. req.params.id .. "/name/edit",
--         }))
--     end)
--
--     -- Cancel endpoint (returns same shape as the initial cell):
--     app.get("/assets/:id/name/view", function(req, res)
--         local asset = db.query("SELECT name FROM assets WHERE id=?",
--                                { req.params.id })[1]
--         res:html(inline_edit.cell({
--             value    = asset.name,
--             edit_url = "/assets/" .. req.params.id .. "/name/edit",
--         }))
--     end)

local inline_edit = {}

-- HTML body / attribute escape (covers both contexts; safest).
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

-- Reject non-string/number values at entry points so a caller
-- passing a table accidentally produces a clear error rather than
-- "table: 0x..." in the output. nil collapses to empty string
-- via esc(); that's intentional.
local function check_scalar(value, name)
    if value == nil then return end
    local t = type(value)
    if t ~= "string" and t ~= "number" then
        error("inline_edit: " .. name .. " must be a string or number, got " .. t, 3)
    end
end

--- Render the display-mode cell (read-only span that swaps to
--- the editor on click).
--
-- Required opts:
--   - `value`     string — current displayed value.
--   - `edit_url`  string — GET endpoint that returns the editor
--                          fragment (see `inline_edit.editor`).
--
-- Optional opts:
--   - `label`     string — accessible name; sets `aria-label` and
--                          `title` so keyboard / screen-reader
--                          users know what activating the cell
--                          will do. Default `"Edit"`.
--
-- The span uses `outerHTML` swap so the entire cell is replaced
-- by the editor fragment on click. `role="button"` + `tabindex=0`
-- make it keyboard-activatable; Enter / Space on the focused
-- span fires the click (browser default for role=button).
--
-- @tparam table opts
-- @treturn string  HTML, ready for `| raw` splicing.
function inline_edit.cell(opts)
    opts = opts or {}
    check_scalar(opts.value,    "value")
    check_scalar(opts.edit_url, "edit_url")
    check_scalar(opts.label,    "label")
    local value    = esc(opts.value)
    local edit_url = esc(opts.edit_url)
    local label    = esc(opts.label or "Edit")
    return '<span class="hull-inline-edit-view" role="button" tabindex="0"'
        .. ' aria-label="' .. label .. '" title="' .. label .. '"'
        .. ' hx-get="' .. edit_url .. '"'
        .. ' hx-trigger="click, keyup[key==\'Enter\'] from:this, keyup[key==\' \'] from:this"'
        .. ' hx-swap="outerHTML">'
        .. value
        .. '</span>'
end

--- Render the edit-mode form (input + Save + Cancel buttons).
--
-- Required opts:
--   - `value`       string — current value to pre-fill the input.
--   - `save_url`    string — PATCH endpoint that persists the
--                            new value and returns the display
--                            fragment.
--   - `cancel_url`  string — GET endpoint that returns the
--                            display fragment unchanged (used by
--                            both the Cancel button and Esc).
--
-- Optional opts:
--   - `name`        string — form-field name. Default `"value"`.
--                            Server reads `req.body[name]`.
--   - `label`       string — `aria-label` for the input.
--                            Default `"Edit value"`.
--   - `save_label`  string — text on the Save button.
--                            Default `"Save"`.
--   - `cancel_label` string — text on the Cancel button.
--                            Default `"Cancel"`.
--
-- Submit (Enter or Save button) PATCHes save_url; the server
-- response replaces the form via outerHTML swap. Cancel button
-- (or Esc on the input) GETs cancel_url for the same swap.
--
-- @tparam table opts
-- @treturn string  HTML, ready for `| raw` splicing.
function inline_edit.editor(opts)
    opts = opts or {}
    check_scalar(opts.value,        "value")
    check_scalar(opts.save_url,     "save_url")
    check_scalar(opts.cancel_url,   "cancel_url")
    check_scalar(opts.name,         "name")
    check_scalar(opts.label,        "label")
    check_scalar(opts.save_label,   "save_label")
    check_scalar(opts.cancel_label, "cancel_label")
    local value        = esc(opts.value)
    local save_url     = esc(opts.save_url)
    local cancel_url   = esc(opts.cancel_url)
    local name         = esc(opts.name or "value")
    local label        = esc(opts.label or "Edit value")
    local save_label   = esc(opts.save_label or "Save")
    local cancel_label = esc(opts.cancel_label or "Cancel")
    -- Esc-to-cancel: htmx trigger on the form watches keyup
    -- bubbling from anywhere within (Esc on the input cancels).
    return '<form class="hull-inline-edit-form"'
        .. ' hx-patch="' .. save_url .. '"'
        .. ' hx-target="this" hx-swap="outerHTML">'
        .. '<input type="text" name="' .. name .. '"'
        .. ' value="' .. value .. '" aria-label="' .. label .. '">'
        .. '<button type="submit" class="hull-inline-edit-save">'
        .. save_label .. '</button>'
        .. '<button type="button" class="hull-inline-edit-cancel"'
        .. ' hx-get="' .. cancel_url .. '"'
        .. ' hx-trigger="click, keyup[key==\'Escape\'] from:closest form"'
        .. ' hx-target="closest form" hx-swap="outerHTML">'
        .. cancel_label .. '</button>'
        .. '</form>'
end

return inline_edit
