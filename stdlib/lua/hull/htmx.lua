--- HTMX request inspection and response-header helpers.
--
-- @module hull.htmx
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
-- @tparam table req   Request object (used to detect htmx).
-- @tparam table res   Response object.
-- @tparam string path Target URL.
function htmx.redirect(req, res, path)
    if htmx.is(req) then
        res:header("HX-Redirect", path)
        res:status(204)
        res:send("")
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
-- `HX-Trigger` value can be a bare event name (`"saved"`) or a JSON
-- object mapping event names to payloads (`{ saved = { id = 42 } }`).
-- The payload form is passed through as JSON; the helper does the
-- encoding when a table is given.
--
-- The triggered events fire on the swap target. Use
-- `htmx.trigger_after_swap` / `htmx.trigger_after_settle` for events
-- that should fire later in the htmx lifecycle.
--
-- @tparam table        res             Response object.
-- @tparam string       event_or_table  Event name OR a table of name→payload.
-- @tparam any|nil      payload         Payload for a bare event name.
function htmx.trigger(res, event_or_table, payload)
    htmx._trigger(res, "HX-Trigger", event_or_table, payload)
end

--- Trigger events after the swap completes.
--
-- @tparam table  res             Response object.
-- @tparam string event_or_table  Event name OR table.
-- @tparam any|nil payload
function htmx.trigger_after_swap(res, event_or_table, payload)
    htmx._trigger(res, "HX-Trigger-After-Swap", event_or_table, payload)
end

--- Trigger events after the swap settles.
--
-- @tparam table  res             Response object.
-- @tparam string event_or_table  Event name OR table.
-- @tparam any|nil payload
function htmx.trigger_after_settle(res, event_or_table, payload)
    htmx._trigger(res, "HX-Trigger-After-Settle", event_or_table, payload)
end

-- Shared encoder for the three HX-Trigger* headers.
function htmx._trigger(res, header_name, event_or_table, payload)
    local value
    if type(event_or_table) == "table" then
        value = require("hull.json").encode(event_or_table)
    elseif payload ~= nil then
        value = require("hull.json").encode({ [event_or_table] = payload })
    else
        value = event_or_table
    end
    res:header(header_name, value)
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
-- @tparam table       res  Response object.
-- @tparam string|bool url  URL to push, or `false` to suppress.
function htmx.push_url(res, url)
    if url == false then
        res:header("HX-Push-Url", "false")
    else
        res:header("HX-Push-Url", url)
    end
end

--- Replace the current URL in the browser history.
--
-- Sets `HX-Replace-Url`. Like `push_url` but uses
-- `history.replaceState` instead of `pushState`.
--
-- @tparam table       res  Response object.
-- @tparam string|bool url  URL to replace, or `false` to suppress.
function htmx.replace_url(res, url)
    if url == false then
        res:header("HX-Replace-Url", "false")
    else
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
-- @tparam table         res             Response object.
-- @tparam string|table  path_or_opts    Path string OR context table.
function htmx.location(res, path_or_opts)
    local value
    if type(path_or_opts) == "table" then
        value = require("hull.json").encode(path_or_opts)
    else
        value = path_or_opts
    end
    res:header("HX-Location", value)
end

return htmx
