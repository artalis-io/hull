-- Tests for both plain HTML and HTMX request paths.
-- All requests use opts.middleware = true so the full middleware chain
-- (CSP nonce, session bootstrap, CSRF) runs; without it test.get/post
-- dispatches the handler directly and skips middleware.

test("GET / returns full HTML page", function()
    local res = test.get("/", { middleware = true })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "<!doctype html>"), "should be a full page")
    test.ok(string.find(res.body, "id=\"entries\""), "should contain entry list")
    -- Loading indicator wired up at the body level + spinner div present.
    test.ok(string.find(res.body, 'hx-indicator="#spinner"', 1, true),
            "body should declare hx-indicator")
    test.ok(string.find(res.body, 'id="spinner"', 1, true),
            "spinner div should be in the page")
end)

test("POST /entries with hx-request returns fragment, not redirect", function()
    -- Get a CSRF token + session cookie by hitting / first.
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    test.ok(token and #token > 0, "csrf token should be in form")
    -- Send htmx-flavored POST with token + cookie.
    local res = test.post("/entries", {
        middleware = true,
        body = "title=buy+milk&_csrf=" .. token,
        headers = {
            ["hx-request"] = "true",
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = home.headers["set-cookie"] or "",
            ["x-csrf-token"] = token,
        },
    })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "buy milk"), "fragment should contain new entry")
    test.ok(not string.find(res.body, "<!doctype"), "fragment must NOT be full page")
end)

test("HTMX POST /entries fires flash trigger via HX-Trigger header", function()
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local res = test.post("/entries", {
        middleware = true,
        body = "title=eggs&_csrf=" .. token,
        headers = {
            ["hx-request"] = "true",
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = home.headers["set-cookie"] or "",
            ["x-csrf-token"] = token,
        },
    })
    test.eq(res.status, 200)
    local trig = res.headers["hx-trigger"]
    test.ok(trig and trig:find('"flash"', 1, true),
            "HX-Trigger should carry flash event, got: " .. tostring(trig))
    test.ok(trig:find("Added: eggs", 1, true),
            "flash payload should include the message")
end)

test("plain POST /entries sets session flash; next GET renders it", function()
    -- POST without hx-request → redirect path → flash.set
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local cookie_hdr = home.headers["set-cookie"] or ""
    local post = test.post("/entries", {
        middleware = true,
        body = "title=plain+post+entry&_csrf=" .. token,
        headers = {
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = cookie_hdr,
            ["x-csrf-token"] = token,
        },
    })
    -- Redirect path on plain form post.
    test.ok(post.status == 302 or post.status == 303,
            "plain POST should redirect, got: " .. tostring(post.status))
    -- Next GET on same session should render the flash.
    local next_get = test.get("/", {
        middleware = true,
        headers = { ["cookie"] = cookie_hdr },
    })
    test.eq(next_get.status, 200)
    test.ok(string.find(next_get.body, "Added: plain post entry"),
            "next render should include flash message")
    -- And the message is one-shot — a SECOND GET should not contain it.
    local third_get = test.get("/", {
        middleware = true,
        headers = { ["cookie"] = cookie_hdr },
    })
    test.ok(not string.find(third_get.body, "Added: plain post entry"),
            "flash is one-shot; second render must not include it")
end)

test("GET / paginates with ?page=N (default per_page=3 in this demo)", function()
    -- Seed enough entries with distinctively-named titles so substring
    -- matches don't false-positive (e.g. "entry-1" would also match
    -- "entry-10"). Use uuid-like markers.
    local db = require("hull.db")
    db.exec("DELETE FROM entries")  -- start clean for deterministic ordering
    for i = 1, 7 do
        db.exec("INSERT INTO entries (title, done) VALUES (?, 0)",
                { "pgN-" .. i .. "-end" })
    end

    -- Sanity: inserts are visible.
    local cnt = db.query("SELECT COUNT(*) AS n FROM entries")[1].n
    test.eq(cnt, 7, "expected 7 entries after seed")

    local page1 = test.get("/", { middleware = true })
    test.eq(page1.status, 200)
    test.ok(string.find(page1.body, 'class="pagination"'),
            "page 1 should render pagination nav")
    -- DESC order over pgN-1..pgN-7 → page 1 shows pgN-7, pgN-6, pgN-5
    test.ok(string.find(page1.body, "pgN-7-end", 1, true),
            "page 1 should contain newest entry")
    test.ok(not string.find(page1.body, "pgN-1-end", 1, true),
            "page 1 should NOT contain oldest entry")

    -- Page 2: pgN-4, pgN-3, pgN-2
    local page2 = test.get("/?page=2", { middleware = true })
    test.eq(page2.status, 200)
    test.ok(string.find(page2.body, "pgN-3-end", 1, true),
            "page 2 should contain mid-range entry")
    test.ok(not string.find(page2.body, "pgN-7-end", 1, true),
            "page 2 should NOT contain page-1 entry")

    -- Page 3: pgN-1 (only one row on the last page)
    local page3 = test.get("/?page=3", { middleware = true })
    test.eq(page3.status, 200)
    test.ok(string.find(page3.body, "pgN-1-end", 1, true),
            "page 3 should contain oldest entry")
end)

test("GET /search filters by title (HTMX fragment)", function()
    local db = require("hull.db")
    db.exec("DELETE FROM entries")
    db.exec("INSERT INTO entries (title, done) VALUES ('buy milk', 0)")
    db.exec("INSERT INTO entries (title, done) VALUES ('buy eggs', 0)")
    db.exec("INSERT INTO entries (title, done) VALUES ('write report', 0)")

    local hit = test.get("/search?q=milk",
        { middleware = true, headers = { ["hx-request"] = "true" } })
    test.eq(hit.status, 200)
    test.ok(string.find(hit.body, "buy milk", 1, true),
            "matching entry should be in result")
    test.ok(not string.find(hit.body, "report", 1, true),
            "non-matching entry should NOT be in result")

    local miss = test.get("/search?q=zzzzz",
        { middleware = true, headers = { ["hx-request"] = "true" } })
    test.eq(miss.status, 200)
    test.ok(string.find(miss.body, "No entries match", 1, true),
            "empty result should render the empty-state row")

    -- Empty query returns all (up to 20).
    local all = test.get("/search?q=",
        { middleware = true, headers = { ["hx-request"] = "true" } })
    test.eq(all.status, 200)
    test.ok(string.find(all.body, "buy milk", 1, true))
    test.ok(string.find(all.body, "buy eggs", 1, true))
    test.ok(string.find(all.body, "write report", 1, true))
end)

test("inline edit: GET /entries/:id/edit returns form, PATCH saves + returns row", function()
    -- Seed one entry we can edit.
    local db = require("hull.db")
    db.exec("DELETE FROM entries")
    db.exec("INSERT INTO entries (title, done) VALUES ('original title', 0)")
    local id = db.query("SELECT id FROM entries LIMIT 1")[1].id

    -- Get a CSRF token (and session cookie) by hitting /.
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local cookie_hdr = home.headers["set-cookie"] or ""
    local hx_headers = {
        ["hx-request"] = "true",
        ["cookie"] = cookie_hdr,
        ["x-csrf-token"] = token,
    }

    -- GET /entries/:id/edit -> inline edit form fragment.
    local edit = test.get("/entries/" .. id .. "/edit",
        { middleware = true, headers = hx_headers })
    test.eq(edit.status, 200)
    test.ok(string.find(edit.body, 'hx-patch="/entries/' .. id .. '"', 1, true),
            "edit form should use hx-patch")
    test.ok(string.find(edit.body, 'value="original title"'),
            "edit form should pre-fill current title")

    -- PATCH /entries/:id -> save + return updated row fragment.
    local patched_headers = {}
    for k, v in pairs(hx_headers) do patched_headers[k] = v end
    patched_headers["content-type"] = "application/x-www-form-urlencoded"
    local saved = test.patch("/entries/" .. id, {
        middleware = true,
        body = "title=updated+title&_csrf=" .. token,
        headers = patched_headers,
    })
    test.eq(saved.status, 200)
    test.ok(string.find(saved.body, "updated title", 1, true),
            "PATCH response should contain new title")
    test.ok(not string.find(saved.body, 'hx-patch="', 1, true),
            "PATCH success should return the row, NOT the edit form")

    -- Confirm DB persisted.
    local stored = db.query(
        "SELECT title FROM entries WHERE id = ?", { id })[1].title
    test.eq(stored, "updated title")

    -- GET /entries/:id -> single row fragment (cancel-edit path).
    local cancel = test.get("/entries/" .. id,
        { middleware = true, headers = hx_headers })
    test.eq(cancel.status, 200)
    test.ok(string.find(cancel.body, "updated title", 1, true))
    test.ok(not string.find(cancel.body, 'hx-patch="', 1, true),
            "GET /entries/:id should return the row, not the edit form")
end)

test("inline edit: PATCH with empty title re-renders edit form with error", function()
    local db = require("hull.db")
    db.exec("DELETE FROM entries")
    db.exec("INSERT INTO entries (title, done) VALUES ('keep me', 0)")
    local id = db.query("SELECT id FROM entries LIMIT 1")[1].id

    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local cookie_hdr = home.headers["set-cookie"] or ""

    local res = test.patch("/entries/" .. id, {
        middleware = true,
        body = "title=&_csrf=" .. token,
        headers = {
            ["hx-request"] = "true",
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = cookie_hdr,
            ["x-csrf-token"] = token,
        },
    })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "Title cannot be empty"),
            "validation error should be in the body")
    test.ok(string.find(res.body, 'hx-patch="/entries/' .. id .. '"', 1, true),
            "response should re-render the edit form")

    -- DB unchanged.
    local stored = db.query(
        "SELECT title FROM entries WHERE id = ?", { id })[1].title
    test.eq(stored, "keep me")
end)

test("idempotency: repeating POST with same Idempotency-Key writes once", function()
    local db = require("hull.db")
    db.exec("DELETE FROM entries")

    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local cookie_hdr = home.headers["set-cookie"] or ""
    local body = "title=idem+probe&_csrf=" .. token
    local headers = {
        ["hx-request"] = "true",
        ["content-type"] = "application/x-www-form-urlencoded",
        ["cookie"] = cookie_hdr,
        ["x-csrf-token"] = token,
        ["idempotency-key"] = "test-key-001",
    }

    -- First submit: real INSERT.
    local first = test.post("/entries", {
        middleware = true, body = body, headers = headers,
    })
    test.eq(first.status, 200)

    -- Second submit with SAME key + SAME body: replayed, no second insert.
    local second = test.post("/entries", {
        middleware = true, body = body, headers = headers,
    })
    test.eq(second.status, 200)
    -- Cached responses carry an X-Idempotency-Replay header.
    test.eq(second.headers["x-idempotency-replay"], "true",
            "second submit should be a replay")

    -- DB still has exactly one row.
    local cnt = db.query("SELECT COUNT(*) AS n FROM entries")[1].n
    test.eq(cnt, 1, "should not have double-inserted")
end)

test("POST /entries with empty title re-renders form with error", function()
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local res = test.post("/entries", {
        middleware = true,
        body = "title=&_csrf=" .. token,
        headers = {
            ["hx-request"] = "true",
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = home.headers["set-cookie"] or "",
            ["x-csrf-token"] = token,
        },
    })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "cannot be empty"), "should show validation error")
    test.eq(res.headers["hx-retarget"], "#new-entry",
            "should retarget to the form, not the list")
    -- Form re-rendered (not just the error text): the input element is back.
    test.ok(string.find(res.body, 'name="title"', 1, true),
            "should re-render the form with the title input")
    test.ok(string.find(res.body, 'aria-invalid="true"', 1, true),
            "field should be marked aria-invalid")
end)

test("POST /entries with too-long title preserves submitted value in re-render", function()
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    -- 201 chars, exceeds the max=200 validation rule.
    local long_title = string.rep("a", 201)
    local res = test.post("/entries", {
        middleware = true,
        body = "title=" .. long_title .. "&_csrf=" .. token,
        headers = {
            ["hx-request"] = "true",
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = home.headers["set-cookie"] or "",
            ["x-csrf-token"] = token,
        },
    })
    test.eq(res.status, 200)
    -- Submitted value preserved in the input via {{ values.title }}.
    test.ok(string.find(res.body, 'value="' .. long_title .. '"', 1, true),
            "submitted (too-long) value should be pre-filled in the input")
    -- Some error message present.
    test.ok(string.find(res.body, '<small role="alert"', 1, true),
            "should show a per-field error message")
end)
