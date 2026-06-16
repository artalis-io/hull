-- HTMX + Pico hypermedia app scaffold.
-- Returns full pages for plain navigation; returns fragments when
-- HX-Request is set. CSRF + per-request CSP nonce wired in by default.
local htmx        = require("hull.web.htmx")
local htmx_confirm = require("hull.web.htmx.confirm")
local htmx_search  = require("hull.web.htmx.search")
local htmx_form    = require("hull.web.htmx.form")
local flash       = require("hull.web.flash")
local pagination  = require("hull.web.pagination")
local validate    = require("hull.validate")
local csp         = require("hull.web.middleware.csp")
local csrf        = require("hull.web.middleware.csrf")
local session     = require("hull.web.middleware.session")
local idempotency = require("hull.web.middleware.idempotency")
local cookie      = require("hull.web.cookie")
local template    = require("hull.template")
local form        = require("hull.web.form")
local log         = require("hull.log")
local db          = require("hull.db")
local attachment      = require("hull.attachment")
local attachment_serve = require("hull.web.attachment-serve")
local blob        = require("hull.blob")
local time        = require("hull.time")

-- Small default so pagination is visible in the demo with only a
-- handful of entries. Real apps would set this to 20-50.
local PER_PAGE_DEFAULT = 3

app.manifest({
    modules = {
        "hull/web/htmx@1",
        "hull/web/htmx/confirm@1",
        "hull/web/htmx/search@1",
        "hull/web/htmx/form@1",
        "hull/web/flash@1",
        "hull/web/pagination@1",
        "hull/validate@1",
        "hull/web/middleware/csp@1",
        "hull/web/middleware/csrf@1",
        "hull/web/middleware/session@1",
        "hull/web/middleware/idempotency@1",
        "hull/web/cookie@1",
        "hull/template@1",
        "hull/web/form@1",
        "hull/db@1",
        "hull/log@1",
        -- Photo attachments (§1.5.b-5). fs / mime are transitive
        -- (used by blob + attachment internally), not imported here.
        -- http-server is intrinsic via app.get/post/... — never put
        -- it in modules.
        "hull/attachment@1",
        "hull/web/attachment-serve@1",
        "hull/blob@1",
        "hull/time@1",
    },
    -- Attachments live under data/. blob.init creates data/blobs/ on
    -- first run; the rest is hull-managed.
    fs = { write = { "data/" } },
})

-- Pre-rendered widget attribute strings. Hull's template engine
-- supports nil-safe dot paths but NOT function calls, so widget
-- helpers are called once at handler time and the resulting
-- strings flow into the template data. Pattern: the constants
-- below are static across requests (same widget config every
-- render), so building them once at load time is enough.
local DELETE_CONFIRM_ATTRS = htmx_confirm.attrs(
    "Delete this entry? (photos will be removed too)",
    { danger = true, yes = "Delete" })

local SEARCH_INPUT_ATTRS = htmx_search.input_attrs({
    url      = "/search",
    target   = "#entry-feed",
    push_url = true,
})

-- Sessions table (creates _hull_sessions on first run).
session.init({ ttl = 86400 })

-- Idempotency-key cache (creates _hull_idempotency_keys on first run).
-- Only kicks in when the client sends an `Idempotency-Key: <uuid>` header
-- — without it, requests pass through normally. HTMX doesn't send the
-- header by default; opt in with `hx-headers='{"Idempotency-Key":"..."}'`
-- on the form. See docs/htmx.md § Idempotency for the client recipe.
idempotency.init({ ttl = 86400 })

-- Photo attachments. blob.init + attachment.init have to run after
-- the sandbox wires the fs cap, so they go inside app.main (which
-- fires once on the event-loop thread, then control falls through
-- to the serve loop because we have handlers registered). The MIME
-- allowlist gates uploads to the four common photo formats; size
-- cap of 4 MiB is generous for typical phone shots while protecting
-- against accidental large uploads from a misconfigured client.
app.main(function()
    blob.init({ dir = "data/blobs" })
    attachment.init({
        max_size = 4 * 1024 * 1024,
        mime_allowlist = { "image/png", "image/jpeg", "image/gif", "image/webp" },
    })
end)

-- CSRF secret. CHANGE-ME-IN-PRODUCTION is the placeholder shipped by
-- the scaffold; load from env (or a sealed secret) before deploying.
-- The scaffold logs a one-time warning at startup if it's still the
-- placeholder so a deploy can't silently inherit it.
local CSRF_SECRET = "CHANGE-ME-IN-PRODUCTION"
if CSRF_SECRET == "CHANGE-ME-IN-PRODUCTION" then
    log.warn("WARNING: CSRF secret is the scaffold placeholder. " ..
             "Replace `CSRF_SECRET` in app.lua with a real high-entropy " ..
             "value before deploying (load from env, etc.).")
end

-- Bootstrap an anonymous session on every request. CSRF binds to
-- this session id; without one, csrf.generate would fail. Creates a
-- new session on first visit (sets cookie); loads existing on
-- subsequent requests. Production apps would replace this with a
-- real login flow (auth.session_middleware).
--
-- `secure = false` on the cookie is intentional for the scaffold: it
-- lets `hull dev` (plain HTTP on :8080) work in a real browser.
-- Production over HTTPS should pass `{ secure = true }` so the cookie
-- is only sent over TLS.
local function session_bootstrap(req, res)
    req.ctx = req.ctx or {}
    local cookies = cookie.parse(req.headers["cookie"] or "")
    local sid = cookies.hull_session
    if sid and session.load(sid) then
        req.ctx.session_id = sid
        return 0
    end
    sid = session.create({})
    req.ctx.session_id = sid
    res:header("Set-Cookie", cookie.serialize("hull_session", sid, { secure = false }))
    return 0
end

-- Pre-body middleware (runs before req.body is read):
--   1. CSP nonce. Populates req.ctx.csp_nonce + sets the header.
--   2. Session bootstrap. Populates req.ctx.session_id.
-- Helper: load attachments for a entry. The _hull_attachments table
-- is internal (capability layer blocks direct access), so we go
-- through attachment.metadata(id). Used wherever a single row is
-- rendered so the photo strip survives toggle / edit / patch.
local function _attachments_for(entry_id)
    local refs = db.query(
        "SELECT attachment_id FROM entry_attachments WHERE entry_id = ? "
        .. "ORDER BY created_at DESC",
        { entry_id })
    local out = {}
    for _, r in ipairs(refs or {}) do
        local meta = attachment.metadata(r.attachment_id)
        if meta then out[#out + 1] = meta end
    end
    return out
end

-- Bulk-load attachments for a batch of entry ids in ONE join query
-- (vs. N per-row SELECTs). Returns a map entry_id -> attachments[].
-- Still does one attachment.metadata() call per attachment because
-- the metadata table is namespaced-internal and only that API is
-- exposed; for a feed of 3 entries with at most a few photos each
-- this is still a big win over N+1.
local function _attachments_for_many(entry_ids)
    local result = {}
    if not entry_ids or #entry_ids == 0 then return result end
    for _, id in ipairs(entry_ids) do result[id] = {} end
    local placeholders = {}
    for i = 1, #entry_ids do placeholders[i] = "?" end
    local rows = db.query(
        "SELECT entry_id, attachment_id FROM entry_attachments "
        .. "WHERE entry_id IN (" .. table.concat(placeholders, ",") .. ") "
        .. "ORDER BY entry_id, created_at DESC",
        entry_ids)
    for _, r in ipairs(rows or {}) do
        local meta = attachment.metadata(r.attachment_id)
        if meta then table.insert(result[r.entry_id], meta) end
    end
    return result
end

-- Coerce SQLite's INTEGER done (0/1) to a Lua boolean and load the
-- attachment list. The template engine doesn't support `==` and Lua
-- treats integer 0 as TRUTHY (only nil/false are falsy), so without
-- this coercion the checkbox would render `checked` forever.
-- Mutates the row in place + returns it for chaining convenience.
local function _hydrate_row(row)
    row.done = (row.done == 1) or row.done == true
    row.attachments = _attachments_for(row.id)
    return row
end

-- Hydrate a db row + return template data (csrf_token comes from req).
-- delete_confirm_attrs is the pre-rendered string for the delete
-- button's `hx-confirm` + `data-confirm-*` set (see top-of-file
-- constant). Same string on every row; cheap to splice.
local function _row_data(row, req)
    return {
        t                    = _hydrate_row(row),
        csrf_token           = req.ctx.csrf_token,
        delete_confirm_attrs = DELETE_CONFIRM_ATTRS,
    }
end

app.use("*", "/*", csp.htmx())
app.use("*", "/*", session_bootstrap)

-- Post-body middleware (runs after req.body is parsed):
--   3. CSRF. Verifies on unsafe methods; injects token on safe.
--   4. Idempotency. Only takes effect when the client sends an
--      `Idempotency-Key` header (HTMX double-clicks land here when
--      the form opts in via `hx-headers`). Scoped to mutating
--      methods so safe GETs don't pay the dispatcher cost.
app.use_post("*",     "/*",     csrf.middleware({ secret = CSRF_SECRET }))
app.use_post("POST",  "/*",     idempotency.middleware({
    get_principal = function(req) return req.ctx.session_id or "__anon" end,
}))
app.use_post("PATCH", "/*",     idempotency.middleware({
    get_principal = function(req) return req.ctx.session_id or "__anon" end,
}))

-- Coerce a route param like "42" into a positive integer, or nil if
-- it isn't one. Mirrors JS's `Number.isInteger(id) && id >= 1` guard
-- so neither runtime ever passes garbage straight to SQL.
local function _valid_id(s)
    local n = tonumber(s)
    if n and n >= 1 and n == math.floor(n) then return n end
    return nil
end

-- Percent-encode for use as a query-string value.
local function _url_encode(s)
    return (s:gsub("([^A-Za-z0-9%-._~])", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

-- Build the data needed by partials/_entry_feed.html (entries + pagination)
-- plus the page-level extras (csrf_token + csp_nonce). Used by both the
-- full-page GET / and the fragment-only GET /search.
local function _feed_data(req, q)
    local p = pagination.from_query(req, {
        default_per_page = PER_PAGE_DEFAULT,
    })
    local total, entries
    if q == "" then
        total = db.query("SELECT COUNT(*) AS n FROM entries")[1].n
        entries = db.query(
            "SELECT id, title, done FROM entries ORDER BY id DESC "
            .. "LIMIT ? OFFSET ?",
            { p.limit, p.offset })
    else
        -- LIKE-escape % _ \ so a query of "100%" matches the literal
        -- "100%" instead of "everything after 100". ESCAPE '\' tells
        -- SQLite which char is our escape. SQL injection is blocked
        -- by parameterization separately.
        local esc = q:gsub("[\\%%_]", "\\%0")
        local pattern = "%" .. esc .. "%"
        total = db.query(
            "SELECT COUNT(*) AS n FROM entries "
            .. "WHERE title LIKE ? ESCAPE '\\'", { pattern })[1].n
        entries = db.query(
            "SELECT id, title, done FROM entries "
            .. "WHERE title LIKE ? ESCAPE '\\' "
            .. "ORDER BY id DESC LIMIT ? OFFSET ?",
            { pattern, p.limit, p.offset })
    end
    -- Coerce `done` (Lua 0-is-truthy gotcha) + batch-load attachments
    -- in one join query instead of N per-row SELECTs.
    local ids = {}
    for _, t in ipairs(entries) do
        t.done = (t.done == 1) or t.done == true
        ids[#ids + 1] = t.id
    end
    local atts = _attachments_for_many(ids)
    for _, t in ipairs(entries) do t.attachments = atts[t.id] or {} end

    -- Pagination links must preserve the current query so that
    -- clicking page 2 of a filtered list stays filtered.
    local base = "/search"
    if q ~= "" then base = base .. "?q=" .. _url_encode(q) end
    local nav = pagination.render(total, {
        page             = p.page,
        per_page         = p.per_page,
        default_per_page = PER_PAGE_DEFAULT,
        base_url         = base,
    })

    return {
        csrf_token = req.ctx.csrf_token,
        csp_nonce  = req.ctx.csp_nonce,
        q          = q,
        has_query  = q ~= "",
        entries      = entries,
        has_entries  = #entries > 0,
        pagination = nav,
        -- Pre-rendered widget strings consumed by the partials
        -- below (templates can't call functions; widgets pre-
        -- render in the handler and the strings flow through).
        search_input_attrs   = SEARCH_INPUT_ATTRS,
        delete_confirm_attrs = DELETE_CONFIRM_ATTRS,
    }
end

app.get("/", function(req, res)
    -- flash.consume drains any pending one-shot messages from the
    -- previous POST/redirect/GET cycle and clears them from session.
    local msgs = flash.consume(req)

    local q = ((req.query and req.query.q) or "")
                :gsub("^%s+", ""):gsub("%s+$", "")

    local data = _feed_data(req, q)
    data.flash     = msgs
    data.has_flash = #msgs > 0
    -- Initial-page render: no validation errors, so the form
    -- widget pre-rendered strings are both empty (the helpers
    -- return "" for nil errors).
    data.values      = data.values      or {}
    data.title_attrs = htmx_form.field_attrs(nil, "title")
    data.title_error = htmx_form.field_error(nil, "title")
    res:html(template.render("pages/home.html", data))
end)

app.post("/entries", function(req, res)
    local fields = form.parse(req.body or "")

    -- Validate via hull.validate. `trim = true` strips whitespace
    -- into the same `fields` table in-place, so a post-validate
    -- `fields.title` is already the cleaned value.
    local ok, errors = validate.check(fields, {
        title = {
            required = true, trim = true, min = 1, max = 200,
            message = "Title cannot be empty.",
        },
    })
    if not ok then
        -- Re-render the form fragment with submitted values + per-field
        -- error messages. hx-retarget so the response lands on the form
        -- itself (the form's own hx-target is #entries, but the validation
        -- response should replace #new-entry).
        htmx.retarget(res, "#new-entry")
        htmx.reswap(res, "outerHTML")
        res:html(template.render("partials/entry_form.html", {
            csrf_token  = req.ctx.csrf_token,
            values      = fields,
            errors      = errors,
            -- Pre-rendered widget strings: aria-invalid +
            -- aria-describedby on the input, plus the inline
            -- <span> error. Both are empty strings on the
            -- success path where errors is nil/empty — see the
            -- initial-page render below.
            title_attrs = htmx_form.field_attrs(errors, "title"),
            title_error = htmx_form.field_error(errors, "title"),
        }))
        return
    end

    local title = fields.title  -- already trimmed by validate
    db.exec("INSERT INTO entries (title, done) VALUES (?, 0)", { title })

    if htmx.is(req) then
        -- HTMX: return the full feed partial so pagination nav (which
        -- lives INSIDE #entry-feed) refreshes too — otherwise crossing
        -- the per_page threshold leaves stale nav from the last render.
        -- The #new-entry form resets via /static/app.js, independent
        -- of the response. flash.trigger fires a client-side 'flash'
        -- event any listener can render.
        flash.trigger(res, "Added: " .. title, "success")
        local html = template.render("partials/_entry_feed.html",
            _feed_data(req, ""))
        -- idempotency.respond_html caches the rendered HTML so a retry
        -- with the same Idempotency-Key gets the same response without
        -- re-running the handler. No-op when no key is active.
        idempotency.respond_html(req, res, 200, html)
    else
        -- Plain form post: stash a message in session, redirect.
        -- The next GET / will consume + render it.
        flash.set(req, "Added: " .. title, "success")
        res:redirect("/")
    end
end)

-- Paginated search. Hit by the search input's hx-get (debounced
-- keyup) AND by the pagination nav's hx-get links — both target
-- the same #entry-feed wrapper so a single response shape (the
-- _entry_feed.html partial = ul + nav) refreshes BOTH the row list
-- AND the page links in one swap. Plain (non-HTMX) navigation falls
-- through to a redirect to / with the query preserved so back/forward
-- still work.
app.get("/search", function(req, res)
    local q = ((req.query and req.query.q) or "")
                :gsub("^%s+", ""):gsub("%s+$", "")
    if htmx.is(req) then
        local data = _feed_data(req, q)
        res:html(template.render("partials/_entry_feed.html", data))
    else
        local dest = "/"
        if q ~= "" then dest = "/?q=" .. _url_encode(q) end
        res:redirect(dest)
    end
end)

app.post("/entries/:id/toggle", function(req, res)
    local id = _valid_id(req.params.id)
    if not id then res:status(404); return end
    db.exec("UPDATE entries SET done = NOT done WHERE id = ?", { id })
    local row = db.query("SELECT id, title, done FROM entries WHERE id = ?", { id })[1]
    if not row then res:status(404); return end
    if htmx.is(req) then
        res:html(template.render("partials/entry_row.html", _row_data(row, req)))
    else
        res:redirect("/")
    end
end)

-- Inline edit. Triad of routes:
--   GET   /entries/:id/edit   -> swap row to inline edit form
--   GET   /entries/:id        -> show a single row (used by Cancel)
--   PATCH /entries/:id        -> save edit, return row fragment (or
--                               re-render edit form on validation error)
-- Order matters: register the MORE SPECIFIC path (/edit) first.
-- Hull's router is first-match, and the bare /entries/:id pattern
-- would otherwise greedily capture "123/edit" as the :id.
-- Plain-form fallback for PATCH: see docs/htmx.md (Rails-style
-- POST + _method=PATCH override). The example is HTMX-only.

app.get("/entries/:id/edit", function(req, res)
    local id = _valid_id(req.params.id)
    if not id then res:status(404); return end
    local row = db.query(
        "SELECT id, title, done FROM entries WHERE id = ?", { id })[1]
    if not row then res:status(404); return end
    res:html(template.render("partials/_entry_edit_form.html", {
        t = row,
        csrf_token = req.ctx.csrf_token,
    }))
end)

app.get("/entries/:id", function(req, res)
    local id = _valid_id(req.params.id)
    if not id then res:status(404); return end
    local row = db.query(
        "SELECT id, title, done FROM entries WHERE id = ?", { id })[1]
    if not row then res:status(404); return end
    if htmx.is(req) then
        res:html(template.render("partials/entry_row.html", _row_data(row, req)))
    else
        res:redirect("/")
    end
end)

app.patch("/entries/:id", function(req, res)
    local id = _valid_id(req.params.id)
    if not id then res:status(404); return end
    local fields = form.parse(req.body or "")
    local title = (fields.title or "")
                    :gsub("^%s+", ""):gsub("%s+$", "")
    if title == "" then
        -- Re-render the edit form with an inline error. hx-retarget
        -- so the response targets the row even though the form's
        -- hx-target was already #entry-{id}.
        local existing = db.query(
            "SELECT id, title, done FROM entries WHERE id = ?", { id })[1]
        if not existing then res:status(404); return end
        existing.title = ""  -- keep the empty value the user submitted
        res:html(template.render("partials/_entry_edit_form.html", {
            t = existing,
            csrf_token = req.ctx.csrf_token,
            error = "Title cannot be empty.",
        }))
        return
    end
    db.exec("UPDATE entries SET title = ? WHERE id = ?", { title, id })
    local row = db.query(
        "SELECT id, title, done FROM entries WHERE id = ?", { id })[1]
    if not row then res:status(404); return end
    res:html(template.render("partials/entry_row.html", _row_data(row, req)))
end)

app.delete("/entries/:id", function(req, res)
    local id = _valid_id(req.params.id)
    if not id then res:status(404); return end
    -- Drop the join rows first; capture them to also drop the
    -- attachments themselves. attachment.delete handles the refcount
    -- so the on-disk blob unlinks only when no other rows reference it.
    local refs = db.query(
        "SELECT attachment_id FROM entry_attachments WHERE entry_id = ?",
        { id })
    db.exec("DELETE FROM entry_attachments WHERE entry_id = ?", { id })
    for _, r in ipairs(refs or {}) do
        attachment.delete(r.attachment_id)
    end
    db.exec("DELETE FROM entries WHERE id = ?", { id })
    if htmx.is(req) then
        -- Refresh the whole feed so pagination nav reflects the new
        -- total. The button targets #entry-feed innerHTML so the entire
        -- list (rows + nav) is replaced in one swap.
        res:html(template.render("partials/_entry_feed.html",
            _feed_data(req, "")))
    else
        res:redirect("/")
    end
end)

-- ── Photo attachments (§1.5.b-5) ─────────────────────────────────────
-- All routes are scoped under a specific entry. The example pairs an
-- in-process gating story (anonymous-session = owns everything in the
-- demo's single-user model) with the attachment-serve helper's
-- default-deny semantics. A real multi-user app would consult the
-- entry's owner column against req.ctx.session_user_id; for the demo
-- we just enforce that the join row exists.

-- Helper: did the current session post this attachment to this entry?
local function _owns_attachment(entry_id, attachment_id)
    local rows = db.query(
        "SELECT 1 FROM entry_attachments WHERE entry_id = ? AND attachment_id = ?",
        { entry_id, attachment_id })
    return rows and #rows > 0
end

-- POST /entries/:id/photos — file-input upload via multipart/form-data.
app.post("/entries/:id/photos", function(req, res)
    local entry_id = _valid_id(req.params.id)
    if not entry_id then res:status(404); return end
    local exists = db.query("SELECT id FROM entries WHERE id = ?", { entry_id })[1]
    if not exists then res:status(404); return end

    local new_attachments = {}
    local ok, err = pcall(function()
        for part in req:multipart() do
            if part.filename then
                local att_id = attachment.store(part, {
                    uploaded_by = req.ctx.session_id or "anonymous",
                })
                db.exec(
                    "INSERT INTO entry_attachments (entry_id, attachment_id, created_at) VALUES (?, ?, ?)",
                    { entry_id, att_id, time.now() })
                new_attachments[#new_attachments + 1] = att_id
            end
        end
    end)
    if not ok then
        if htmx.is(req) then
            -- Render via template.render_string so {{ err }} is HTML-
            -- escaped. err may carry user-influenced text (filename,
            -- mime, etc.) so raw `..` concat into HTML would be an XSS
            -- sink. flash.set() (else branch) goes through the flash
            -- partial and is auto-escaped already.
            res:status(413):html(template.render_string(
                '<small role="alert" class="error">Upload failed: {{ err }}</small>',
                { err = tostring(err) }))
        else
            flash.set(req, "Upload failed: " .. tostring(err), "error")
            res:redirect("/")
        end
        return
    end

    if htmx.is(req) then
        res:html(template.render("partials/_attachment_strip.html", {
            t = { id = entry_id, attachments = _attachments_for(entry_id) },
        }))
    else
        res:redirect("/")
    end
end, { multipart = { max_part_size = 8 * 1024 * 1024 } })

-- GET /entries/:id/photos/:att_id — auth-gated serve. The default-deny
-- of attachment-serve is overridden here with our own _owns_attachment
-- check (which also gates against malicious cross-entry ID guessing).
app.get("/entries/:id/photos/:att_id", function(req, res)
    local entry_id = _valid_id(req.params.id)
    if not entry_id then res:status(404); return end
    local att_id  = req.params.att_id
    attachment_serve.serve(req, res, att_id, {
        auth_check = function(_, _meta)
            return entry_id and _owns_attachment(entry_id, att_id)
        end,
    })
end)

-- DELETE /entries/:id/photos/:att_id — drop the join row + decrement
-- the attachment refcount. The on-disk blob unlinks automatically
-- when refcount hits 0 (no other entries reference the same content).
app.delete("/entries/:id/photos/:att_id", function(req, res)
    local entry_id = _valid_id(req.params.id)
    if not entry_id then res:status(404); return end
    local att_id  = req.params.att_id
    if not _owns_attachment(entry_id, att_id) then
        res:status(404); return
    end
    db.exec(
        "DELETE FROM entry_attachments WHERE entry_id = ? AND attachment_id = ?",
        { entry_id, att_id })
    attachment.delete(att_id)
    if htmx.is(req) then
        res:html("")  -- htmx swap-mode=delete removes the figure.
    else
        res:redirect("/")
    end
end)

log.info("hypermedia app loaded")
