--- HTMX confirm dialog — server-side helper.
--
-- @module hull.web.htmx.confirm
-- @license AGPL-3.0-or-later
--
-- Replaces htmx's default `window.confirm()` popup with a styled
-- native `<dialog>` element rendered by the shipped client JS at
-- `/static/hull/htmx/confirm/confirm.js`.
--
-- This module provides one helper: `confirm.attrs(question, opts?)`.
-- It returns a pre-formatted, HTML-attribute-escaped string suitable
-- for splicing into a template via `{{ ... | raw }}`. Server has
-- nothing functional to do at request time — the dialog flow is
-- entirely client-side — but apps still declare the module in their
-- manifest for audit visibility.
--
-- Setup:
--
--     -- 1. Declare in your manifest:
--     app.manifest({
--         modules = { "hull/web/htmx@1", "hull/web/htmx/confirm@1" },
--         csp = "htmx",
--     })
--
--     -- 2. Include the assets in your base template:
--     <link rel="stylesheet" href="/static/hull/htmx/confirm/confirm.css">
--     <script src="/static/hull/htmx/confirm/confirm.js" defer></script>
--
--     -- 3. Use on any htmx-triggering element:
--     local confirm = require("hull.web.htmx.confirm")
--     -- In a template:
--     <button hx-delete="/assets/{{ id }}"
--             {{ confirm.attrs('Delete this asset?',
--                              { danger=true, yes='Delete' }) | raw }}>
--       Delete
--     </button>

local confirm = {}

-- HTML attribute-value escaping. Covers the five characters that
-- can break out of a double-quoted attribute value (& and " are
-- the load-bearing ones; <, >, ' included for defense in depth and
-- to render correctly inside any container context).
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

--- Render htmx + data attributes for a confirm dialog.
--
-- Always emits `hx-confirm="<escaped question>"`. The client JS
-- intercepts this and shows the styled dialog instead of the
-- browser's native popup.
--
-- Optional opts:
--
--   - `yes`     string — custom label for the confirm button.
--                       Default "Confirm".
--   - `no`      string — custom label for the cancel button.
--                       Default "Cancel".
--   - `danger`  bool  — when true, emits `data-confirm-danger="true"`
--                       so CSS can style the confirm button as
--                       destructive (red, etc.). Apps wire this
--                       via `dialog#hull-confirm
--                       .hull-confirm-yes[data-danger="true"]`.
--   - `title`   string — optional heading shown above the question.
--
-- All values are HTML-attribute-escaped — caller may pass user-
-- supplied text safely.
--
-- @tparam string question  The yes/no prompt.
-- @tparam[opt] table opts  { yes, no, danger, title }.
-- @treturn string  Attribute string, ready for `| raw` splicing.
function confirm.attrs(question, opts)
    opts = opts or {}
    local parts = { 'hx-confirm="' .. attr_escape(question or "") .. '"' }
    if opts.yes then
        parts[#parts + 1] = 'data-confirm-yes="' .. attr_escape(opts.yes) .. '"'
    end
    if opts.no then
        parts[#parts + 1] = 'data-confirm-no="' .. attr_escape(opts.no) .. '"'
    end
    if opts.danger then
        parts[#parts + 1] = 'data-confirm-danger="true"'
    end
    if opts.title then
        parts[#parts + 1] = 'data-confirm-title="' .. attr_escape(opts.title) .. '"'
    end
    return table.concat(parts, " ")
end

return confirm
