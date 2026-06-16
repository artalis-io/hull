--- HTMX form widget — server-side helpers for validation errors
--- and submit-button loading state.
--
-- @module hull.web.htmx.form
-- @license AGPL-3.0-or-later
--
-- Three small helpers, all consuming the `errors` table returned
-- by `hull.validate.check(data, schema)`:
--
--   - `form.errors(errors)`            -- whole-form summary
--   - `form.field_error(errors, name)` -- per-field inline span
--   - `form.field_attrs(errors, name)` -- a11y attrs for the input
--
-- All output is HTML-escaped. Pass `errors = nil` (the success case)
-- to any helper and you get an empty string back, so the same
-- template renders cleanly on initial GET and after a failed POST.
--
-- The submit-button loading state is shipped as client JS at
-- `/static/hull/htmx/form/form.js`; no server-side helper is
-- needed for it.
--
-- Setup:
--
--     app.manifest({
--         modules = {
--             "hull/web/htmx@1",
--             "hull/web/htmx/form@1",
--             "hull/validate@1",
--         },
--         csp = "htmx",
--     })
--
--     -- base.html:
--     <link rel="stylesheet" href="/static/hull/htmx/form/form.css">
--     <script src="/static/hull/htmx/form/form.js" defer></script>
--
--     -- handler:
--     local ok, errors = validate.check(req.body, schema)
--     if not ok then
--         res:status(422)
--         res:html(template.render("partials/asset_form.html",
--                                  { values = req.body, errors = errors }))
--         return
--     end
--
--     -- partials/asset_form.html:
--     {{ form.errors(errors) | raw }}
--     <div class="field">
--       <label for="name">Name</label>
--       <input id="name" name="name" value="{{ values.name }}"
--              {{ form.field_attrs(errors, "name") | raw }}>
--       {{ form.field_error(errors, "name") | raw }}
--     </div>

local form = {}

-- Minimal HTML body escape — covers the four characters that
-- terminate an element body context. Apostrophe included for the
-- inline-attribute case some downstream templates wrap us in.
local BODY_ESC = {
    ["&"] = "&amp;",
    ["<"] = "&lt;",
    [">"] = "&gt;",
    ['"'] = "&quot;",
    ["'"] = "&#39;",
}
local function esc(s)
    return (tostring(s):gsub('[&<>"\']', BODY_ESC))
end

-- Stable per-field id used by `aria-describedby` <-> error span.
-- Hyphens already safe in id values; we only need to ensure the
-- name component doesn't escape the attribute itself.
local function error_id(name)
    return "hull-form-error-" .. esc(name)
end

--- Render a whole-form error summary as an `<ul role="alert">`.
--
-- One `<li>` per failing field. `role="alert"` causes assistive
-- tech to announce the block when it appears. Returns the empty
-- string when `errors` is nil or empty so the template stays
-- clean on the success path.
--
-- @tparam ?table errors  Field-name -> error-message map.
-- @treturn string  HTML, or "" when there's nothing to render.
function form.errors(errors)
    if not errors or next(errors) == nil then return "" end
    local parts = { '<ul class="hull-form-errors" role="alert">' }
    -- Sort field names for deterministic output (testable; nicer
    -- on diffs; AT users get a stable announcement order).
    local names = {}
    for k in pairs(errors) do names[#names + 1] = k end
    table.sort(names)
    for _, name in ipairs(names) do
        parts[#parts + 1] = '<li><a href="#' .. error_id(name) .. '">'
            .. esc(errors[name]) .. '</a></li>'
    end
    parts[#parts + 1] = '</ul>'
    return table.concat(parts)
end

--- Render a single inline error span for one field.
--
-- The span's `id` is `hull-form-error-<name>` so input elements
-- can reference it via `aria-describedby` (see `field_attrs`).
-- Returns empty string when there's no error for this field.
--
-- @tparam ?table errors  Field-name -> error-message map.
-- @tparam string name    Field name to render the error for.
-- @treturn string  `<span>...</span>` or "".
function form.field_error(errors, name)
    if not errors or not errors[name] then return "" end
    return '<span id="' .. error_id(name)
        .. '" class="hull-form-error">' .. esc(errors[name]) .. '</span>'
end

--- Render the a11y attribute set to splice into an `<input>` tag.
--
-- Returns `aria-invalid="true" aria-describedby="hull-form-error-<name>"`
-- when the field has an error; returns "" otherwise (so the input
-- doesn't carry a stale aria-invalid).
--
-- @tparam ?table errors  Field-name -> error-message map.
-- @tparam string name    Field name.
-- @treturn string  Attribute string, ready for `| raw` splicing.
function form.field_attrs(errors, name)
    if not errors or not errors[name] then return "" end
    return 'aria-invalid="true" aria-describedby="' .. error_id(name) .. '"'
end

return form
