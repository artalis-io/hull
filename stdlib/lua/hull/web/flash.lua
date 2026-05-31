--- One-shot user notifications across POST/redirect/GET.
--
-- @module hull.web.flash
-- @license AGPL-3.0-or-later
--
-- Classic "flash message" pattern: a handler stashes a short string
-- ("Saved.", "Email already exists", ...), the next request reads
-- and clears it. Survives exactly one render. Lives entirely in the
-- session row so it works across redirects.
--
-- Two emission paths:
--
--   1. **Session-backed** (`flash.set` / `flash.consume`). The
--      classic POST → redirect → GET workflow. Requires
--      `hull/web/middleware/session` to be wired so that
--      `req.ctx.session_id` is populated. The flash array lives at
--      `session.flash`; `consume` drains and persists the cleared
--      state.
--
--   2. **HTMX `HX-Trigger`** (`flash.trigger`). For fragment-swap
--      paths that don't redirect. Emits
--      `HX-Trigger: {"flash":{"text":..., "kind":...}}` so the
--      client-side `htmx:on:flash` listener can render a toast.
--      No session needed.
--
-- Composition note: `flash.trigger` sets `HX-Trigger` directly. If
-- the same response also calls `htmx.trigger(...)`, the second call
-- wins. To fire multiple events in one response, use
-- `htmx.trigger(res, { flash = {...}, other_event = {...} })` and
-- skip `flash.trigger`.

local json = require("hull.json")
local session = require("hull.web.middleware.session")

local flash = {}

-- Resolve the current session table from req.ctx, hydrating from
-- session.load() if the middleware hasn't populated it yet. Returns
-- (sess_table, session_id) or (nil, nil) if no session.
local function get_session_ctx(req)
    if not req or not req.ctx or not req.ctx.session_id then
        return nil, nil
    end
    local sid = req.ctx.session_id
    local sess = req.ctx.session
    if not sess then
        sess = session.load(sid)
        if not sess then return nil, nil end
        req.ctx.session = sess
    end
    return sess, sid
end

--- Stash a flash message for the next render.
--
-- Appends `{text=..., kind=...}` to the session's flash array and
-- persists. Multiple calls in one request accumulate; the next
-- `flash.consume` returns them in insertion order.
--
-- @tparam table  req   Current request (must have `req.ctx.session_id`).
-- @tparam string text  Message text (coerced to string).
-- @tparam[opt] string kind  Category tag for styling (e.g. `"success"`,
--   `"error"`, `"info"`, `"warn"`). Default `"info"`.
--
-- @raise Error if no session is wired or `text` is missing.
function flash.set(req, text, kind)
    if text == nil then
        error("flash.set: text is required", 2)
    end
    local sess, sid = get_session_ctx(req)
    if not sess then
        error("flash.set: no session in req.ctx.session_id (wire "
            .. "hull/web/middleware/session first)", 2)
    end
    sess.flash = sess.flash or {}
    table.insert(sess.flash, {
        text = tostring(text),
        kind = kind or "info",
    })
    session.update(sid, sess)
end

--- Drain pending flash messages.
--
-- Returns the messages array (possibly empty) and clears storage.
-- Idempotent on an empty queue. Safe to call without a session
-- (returns `{}`).
--
-- @tparam table req  Current request.
-- @treturn table  Array of `{text, kind}` entries.
--
-- @usage
--   local msgs = flash.consume(req)
--   res:html(template.render("page.html", { flash = msgs, ... }))
function flash.consume(req)
    local sess, sid = get_session_ctx(req)
    if not sess or not sess.flash or #sess.flash == 0 then
        return {}
    end
    local msgs = sess.flash
    sess.flash = {}
    session.update(sid, sess)
    return msgs
end

--- Fire a client-side flash event via `HX-Trigger`.
--
-- Sets the `HX-Trigger` response header to
-- `{"flash":{"text":..., "kind":...}}`. Client-side HTMX fires a
-- DOM event named `flash` with the payload; the app listens with
-- `document.body.addEventListener("flash", e => ...)` and renders
-- a toast. Use for HTMX fragment-swap paths where no redirect happens.
--
-- @tparam table  res   Response object.
-- @tparam string text  Message text.
-- @tparam[opt] string kind  Category tag. Default `"info"`.
--
-- @raise Error on missing `res` or `text`.
function flash.trigger(res, text, kind)
    if res == nil then
        error("flash.trigger: res is required", 2)
    end
    if text == nil then
        error("flash.trigger: text is required", 2)
    end
    local payload = json.encode({
        flash = { text = tostring(text), kind = kind or "info" },
    })
    res:header("HX-Trigger", payload)
end

return flash
