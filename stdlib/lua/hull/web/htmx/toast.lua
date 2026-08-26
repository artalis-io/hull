--- HTMX toast widget - server-side helper.
--
-- @module hull.web.htmx.toast
-- @license AGPL-3.0-or-later
--
-- Sends transient flash messages to the client via the standard
-- `HX-Trigger` header. The shipped client JS at
-- `/static/hull/htmx/toast/toast.js` listens for the `toast` event
-- and renders each message into a stack at the bottom of the page.
--
-- The widget is structural-only by design: this module ships layout
-- + a11y CSS but no colors, borders, shadows, animations, or fonts.
-- Apps style per-level appearance by targeting the `data-level`
-- attribute on each toast item (see structural.css for the DOM
-- contract).
--
-- Setup:
--
--     -- 1. Declare the module in your manifest:
--     app.manifest({
--         modules = { "hull/web/htmx@1", "hull/web/htmx/toast@1" },
--         csp = "htmx",  -- so the shipped /static/ JS can load
--     })
--
--     -- 2. Include the assets in your base template:
--     <link rel="stylesheet" href="/static/hull/htmx/toast/toast.css">
--     <script src="/static/hull/htmx/toast/toast.js" defer></script>
--
--     -- 3. Fire toasts from any handler:
--     local toast = require("hull.web.htmx.toast")
--     toast.success(res, "Asset saved")
--     toast.error(res, "Permission denied")

local htmx = require("hull.web.htmx")

local toast = {}

-- Valid level values. Anything else collapses to "info" so the
-- client can rely on a known set when styling by [data-level].
local LEVELS = {
    info    = true,
    success = true,
    warning = true,
    error   = true,
}

--- Send a toast to the client.
--
-- The `HX-Trigger` header is set to a JSON object the client JS
-- consumes:
--
--     {"toast": {"message": "...", "level": "info", "duration": 4000}}
--
-- If `htmx.trigger` was already used on this response, the headers
-- compose at the htmx-client level (each top-level key becomes its
-- own event).
--
-- @tparam table  res                   Keel response object.
-- @tparam string message               Toast text. Plain string;
--                                      the client renders it via
--                                      `textContent` (HTML safe).
-- @tparam[opt] table opts              { level, duration, id }
-- @tparam[opt] string opts.level       "info" (default), "success",
--                                      "warning", or "error".
--                                      Unknown values collapse to
--                                      "info".
-- @tparam[opt] number opts.duration    Auto-dismiss in ms (default
--                                      4000). 0 means "no auto-
--                                      dismiss".
-- @tparam[opt] string opts.id          Optional stable id for the
--                                      toast (enables client-side
--                                      dedup of repeated triggers).
function toast.show(res, message, opts)
    opts = opts or {}
    local payload = {
        message  = tostring(message or ""),
        level    = LEVELS[opts.level] and opts.level or "info",
    }
    -- Type-narrow + conditional inclusion: keep the wire payload
    -- clean (nil-valued keys are dropped by hull.json anyway, but
    -- being explicit catches accidental shape drift). Documented
    -- contract: duration is ms-number, id is string.
    if type(opts.duration) == "number" then
        payload.duration = opts.duration
    end
    if opts.id ~= nil then
        payload.id = tostring(opts.id)
    end
    htmx.trigger(res, "toast", payload)
end

-- Copy opts so the shorthand's level pin doesn't mutate the caller's
-- table. JS uses object spread for the same reason.
local function with_level(level, opts)
    local out = {}
    if opts then for k, v in pairs(opts) do out[k] = v end end
    out.level = level
    return out
end

--- Shorthand: `toast.show(res, msg, { level = "info" })`.
function toast.info(res, message, opts)
    toast.show(res, message, with_level("info", opts))
end

--- Shorthand: `toast.show(res, msg, { level = "success" })`.
function toast.success(res, message, opts)
    toast.show(res, message, with_level("success", opts))
end

--- Shorthand: `toast.show(res, msg, { level = "warning" })`.
function toast.warning(res, message, opts)
    toast.show(res, message, with_level("warning", opts))
end

--- Shorthand: `toast.show(res, msg, { level = "error" })`.
function toast.error(res, message, opts)
    toast.show(res, message, with_level("error", opts))
end

return toast
