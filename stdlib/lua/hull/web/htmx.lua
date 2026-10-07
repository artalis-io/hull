--- HTMX request inspection and response-header helpers.
--
-- @module hull.web.htmx
-- @license AGPL-3.0-or-later
--
-- Provides the small server-side surface for the HTMX hypermedia
-- pattern: detect whether an inbound request came from htmx (vs a
-- normal browser navigation), then set the response headers HTMX
-- knows how to interpret (`HX-Redirect`, `HX-Retarget`, `HX-Reswap`,
-- `HX-Trigger`, etc.). Pure functions; no state, no I/O.
--
-- All `req` arguments are the Keel request object Hull's dispatcher
-- passes to handlers; all `res` arguments are the Keel response
-- object. HTMX headers are sent unmodified to the htmx client
-- runtime, which acts on them per the htmx specification.
--
-- The canonical use pattern is:
--
--     if htmx.is(req) then
--         -- Render a fragment template
--         res:html(template.render("partials/todo_row.html", data))
--     else
--         -- Render the full page
--         res:html(template.render("pages/todos.html", data))
--     end

local htmx = {}

-- ── Shared HTML escape ────────────────────────────────────────────────
--
-- The htmx widgets render user data into element text and double-quoted
-- attributes, so they all need HTML escaping. It lives here, once, rather
-- than being re-rolled in each widget (where copies had already drifted -
-- some missed the nil guard). Escapes the five HTML metacharacters plus the
-- backtick (unquoted-attribute contexts), matching hull.template.
local _escape_map = {
    ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;",
    ['"'] = "&quot;", ["'"] = "&#39;", ["`"] = "&#96;",
}

--- HTML-escape a value for safe interpolation into element text or a
--- double-quoted attribute. `nil` becomes the empty string.
-- @tparam any s
-- @treturn string
function htmx.escape(s)
    if s == nil then return "" end
    return (tostring(s):gsub("[&<>\"'`]", _escape_map))
end

--- True if the request was sent by the htmx client runtime.
--
-- Checks for `HX-Request: true`. HTMX sets this on every AJAX request
-- it initiates (whether triggered by `hx-get`, `hx-post`, etc.).
-- Plain browser navigation, form posts without htmx, and fetch calls
-- from non-htmx JS do NOT carry this header.
--
-- @tparam table req  The request object.
-- @treturn boolean   `true` if HX-Request header equals "true".
function htmx.is(req)
    if not req or not req.headers then return false end
    return req.headers["hx-request"] == "true"
end

--- True if the request was sent as an htmx boost.
--
-- Boosted requests come from regular `<a>` or `<form>` elements
-- inside an `hx-boost`'d container. The request is htmx-driven but
-- the user's intent was a normal navigation; the server typically
-- returns a fragment that replaces the body.
--
-- @tparam table req  The request object.
-- @treturn boolean   `true` if HX-Boosted header equals "true".
function htmx.boosted(req)
    if not req or not req.headers then return false end
    return req.headers["hx-boosted"] == "true"
end

--- Return the htmx-driver path of the source element, if available.
--
-- HTMX sets `HX-Current-URL` to the page URL the request was made
-- from. Useful for context-aware fragment rendering.
--
-- @tparam table req  The request object.
-- @treturn string|nil  Current URL, or nil if not htmx-driven.
function htmx.current_url(req)
    if not req or not req.headers then return nil end
    return req.headers["hx-current-url"]
end

--- Return the id of the htmx target element, if specified.
--
-- @tparam table req  The request object.
-- @treturn string|nil  Target id, or nil.
function htmx.target(req)
    if not req or not req.headers then return nil end
    return req.headers["hx-target"]
end

--- Return the name of the htmx trigger element, if specified.
--
-- @tparam table req  The request object.
-- @treturn string|nil  Trigger element name, or nil.
function htmx.trigger_name(req)
    if not req or not req.headers then return nil end
    return req.headers["hx-trigger-name"]
end

--- Send a client-side redirect.
--
-- For htmx requests, sets `HX-Redirect`: the htmx client navigates
-- the browser to `path` after receiving the response. For plain
-- requests, falls back to a normal HTTP 302.
--
-- `path` must be an http(s) URL or a scheme-less relative one: htmx
-- assigns HX-Redirect to `location.href`, so a `javascript:` URL (an
-- app's open redirect passing a user value through) ran script. Any
-- other scheme, a control character or whitespace raises. So does a
-- protocol-relative `//host/...`: it is not a relative path but another
-- site (an open redirect when the value comes from the request), unless
-- `opts.allow_protocol_relative` is set.
--
-- @tparam table req   Request object (used to detect htmx).
-- @tparam table res   Response object.
-- @tparam string path Target URL.
-- @tparam[opt] table opts  `{ allow_protocol_relative = true }`.
local function redirect_target_ok(path, opts)
    if type(path) ~= "string" or path == "" then return false end
    if path:find("[%c%s\\]") then return false end
    if path:sub(1, 2) == "//"
       and not (type(opts) == "table" and opts.allow_protocol_relative == true) then
        return false
    end
    local lower = path:lower()
    if lower:sub(1, 7) == "http://" or lower:sub(1, 8) == "https://" then
        return true
    end
    -- Scheme-less: no ":" before the first "/", "?" or "#".
    local head = path:match("^[^/?#]*")
    return not head:find(":", 1, true)
end

function htmx.redirect(req, res, path, opts)
    if not redirect_target_ok(path, opts) then
        error("htmx.redirect: target must be an http(s) URL or a relative path", 2)
    end
    if htmx.is(req) then
        res:header("HX-Redirect", path)
        res:status(204)
        res:text("")
    else
        res:redirect(path)
    end
end

--- Override the target of an htmx swap.
--
-- Setting `HX-Retarget` tells the htmx client to apply the response
-- to a different element than the one originally specified by
-- `hx-target`. Common use: validation errors land in a different
-- container than the success fragment.
--
-- @tparam table res        Response object.
-- @tparam string selector  CSS selector for the new target.
function htmx.retarget(res, selector)
    res:header("HX-Retarget", selector)
end

--- Override the swap mode of an htmx swap.
--
-- `HX-Reswap` accepts the same modes as `hx-swap`: `innerHTML`,
-- `outerHTML`, `beforebegin`, `afterbegin`, `beforeend`, `afterend`,
-- `delete`, `none`.
--
-- @tparam table res   Response object.
-- @tparam string mode Swap mode.
function htmx.reswap(res, mode)
    res:header("HX-Reswap", mode)
end

--- Trigger one or more client-side events.
--
-- The HX-Trigger header value can be a bare event name (`"saved"`)
-- or a JSON object mapping event names to payloads
-- (`{ saved = { id = 42 } }`). The payload form is passed through
-- as JSON; this helper does the encoding when a table is given.
--
-- Three call shapes:
--   htmx.trigger(res, "saved")                          bare event
--   htmx.trigger(res, "saved", { id = 42 })             event + payload
--   htmx.trigger(res, { saved = {...}, refresh = true }) multi-event
--
-- An optional final `opts` table selects when the event fires in
-- the htmx lifecycle:
--   opts.timing = nil     -> HX-Trigger (immediate, default)
--   opts.timing = "swap"  -> HX-Trigger-After-Swap
--   opts.timing = "settle"-> HX-Trigger-After-Settle
--
-- @tparam table        res             Response object.
-- @tparam string|table event_or_table  Event name OR table.
-- @tparam any|nil      payload         Payload for a bare event name.
-- @tparam table|nil    opts            { timing = "swap" | "settle" | nil }.
function htmx.trigger(res, event_or_table, payload, opts)
    -- Allow opts as the 3rd arg when event_or_table is a table
    -- (no per-event payload in that case): trigger(res, {...}, opts).
    if type(event_or_table) == "table" and type(payload) == "table"
       and opts == nil and payload.timing ~= nil then
        opts = payload
        payload = nil
    end

    local header = "HX-Trigger"
    if opts and opts.timing == "swap" then
        header = "HX-Trigger-After-Swap"
    elseif opts and opts.timing == "settle" then
        header = "HX-Trigger-After-Settle"
    end

    local value
    if type(event_or_table) == "table" then
        value = require("hull.json").encode(event_or_table)
    elseif payload ~= nil then
        value = require("hull.json").encode({ [event_or_table] = payload })
    else
        value = event_or_table
    end
    res:header(header, value)
end

--- Trigger a full client-side page refresh.
--
-- Sets `HX-Refresh: true`. The htmx client does a hard reload of
-- the current page (useful after destructive operations).
--
-- @tparam table res  Response object.
function htmx.refresh(res)
    res:header("HX-Refresh", "true")
end

--- Push a new URL into the browser history.
--
-- Sets `HX-Push-Url`. The htmx client uses `history.pushState` to
-- update the visible URL after the swap. Pass `false` to suppress
-- a default push that htmx would otherwise do.
--
-- `url` is held to the rules of @{redirect} (audit 9): an http(s) URL or a
-- scheme-less relative one, no control characters, whitespace or `\`, and
-- no protocol-relative `//host` unless `opts.allow_protocol_relative`. Any
-- other value raises - an app passing a request value through must not hand
-- the client a `javascript:` URL or another site's address.
--
-- @tparam table       res  Response object.
-- @tparam string|bool url  URL to push, or `false` to suppress.
-- @tparam[opt] table  opts `{ allow_protocol_relative = true }`.
function htmx.push_url(res, url, opts)
    if url == false then
        res:header("HX-Push-Url", "false")
    else
        if not redirect_target_ok(url, opts) then
            error("htmx.push_url: url must be an http(s) URL or a relative path", 2)
        end
        res:header("HX-Push-Url", url)
    end
end

--- Replace the current URL in the browser history.
--
-- Sets `HX-Replace-Url`. Like `push_url` but uses
-- `history.replaceState` instead of `pushState`.
--
-- `url` is validated as for @{push_url}.
--
-- @tparam table       res  Response object.
-- @tparam string|bool url  URL to replace, or `false` to suppress.
-- @tparam[opt] table  opts `{ allow_protocol_relative = true }`.
function htmx.replace_url(res, url, opts)
    if url == false then
        res:header("HX-Replace-Url", "false")
    else
        if not redirect_target_ok(url, opts) then
            error("htmx.replace_url: url must be an http(s) URL or a relative path", 2)
        end
        res:header("HX-Replace-Url", url)
    end
end

--- Issue a client-side navigation without a hard reload.
--
-- Sets `HX-Location`. Value can be a plain path (string) or a
-- table with htmx LocationContext fields (`{ path = "/x",
-- target = "#main", swap = "outerHTML" }`). The htmx client
-- performs a navigation as if the user had triggered it.
--
-- The path (the string, or the table's `path`) is validated as for
-- @{redirect}; anything else raises (audit 9).
--
-- @tparam table         res             Response object.
-- @tparam string|table  path_or_opts    Path string OR context table.
-- @tparam[opt] table    opts            `{ allow_protocol_relative = true }`.
function htmx.location(res, path_or_opts, opts)
    local value
    local path = path_or_opts
    if type(path_or_opts) == "table" then path = path_or_opts.path end
    if not redirect_target_ok(path, opts) then
        error("htmx.location: path must be an http(s) URL or a relative path", 2)
    end
    if type(path_or_opts) == "table" then
        value = require("hull.json").encode(path_or_opts)
    else
        value = path_or_opts
    end
    res:header("HX-Location", value)
end

return htmx
