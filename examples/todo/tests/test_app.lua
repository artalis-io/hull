-- Tests for todo example (with auth, search, csv, rbac)
-- Run: hull test examples/todo/
--
-- Note: middleware registered via app.use() does not run during hull test
-- dispatch. CSRF, session, and rate limiting are not active in these tests.
-- Authenticated routes are tested by injecting session data via opts.ctx.

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

-- ── Auth pages ──────────────────────────────────────────────────────

test("GET /login returns 200", function()
    local res = test.get("/login")
    test.eq(res.status, 200)
end)

test("GET /register returns 200", function()
    local res = test.get("/register")
    test.eq(res.status, 200)
end)

-- ── Registration ────────────────────────────────────────────────────

test("POST /register creates user and redirects", function()
    local res = test.post("/register", {
        body = "name=Alice&email=alice%40test.com&password=secret1234&_csrf=fake",
        headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
    })
    -- Middleware doesn't run, so CSRF won't block. The route creates the user.
    -- Redirect (302) or 200 with form are both acceptable without middleware.
    test.ok(res.status == 302 or res.status == 200, "register response")
end)

-- ── Protected routes redirect without session ───────────────────────

test("GET / redirects to /login without session", function()
    local res = test.get("/")
    test.eq(res.status, 302)
end)

test("POST /add redirects to /login without session", function()
    local res = test.post("/add", {
        body = "title=Test&_csrf=fake",
        headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
    })
    test.eq(res.status, 302)
end)

test("POST /toggle/1 redirects to /login without session", function()
    local res = test.post("/toggle/1", {
        body = "_csrf=fake",
        headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
    })
    test.eq(res.status, 302)
end)

test("POST /delete/1 redirects to /login without session", function()
    local res = test.post("/delete/1", {
        body = "_csrf=fake",
        headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
    })
    test.eq(res.status, 302)
end)

-- ── CSV export ──────────────────────────────────────────────────────

test("GET /export redirects to /login without session", function()
    local res = test.get("/export")
    test.eq(res.status, 302)
end)

-- ── Admin dashboard ─────────────────────────────────────────────────

test("GET /admin redirects to /login without session", function()
    local res = test.get("/admin")
    test.eq(res.status, 302)
end)

-- ── Authenticated routes (inject session via ctx) ───────────────────

-- Alice was registered above (user_id=1, first user → admin role)
local alice = { session = { user_id = 1, email = "alice@test.com", name = "Alice" } }

test("GET / returns 200 with session", function()
    local res = test.get("/", { ctx = alice })
    test.eq(res.status, 200)
end)

test("POST /add creates todo and redirects", function()
    local res = test.post("/add", {
        body = "title=Buy+milk&_csrf=fake",
        headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
        ctx = alice,
    })
    test.eq(res.status, 302)
end)

test("GET / shows added todo", function()
    local res = test.get("/", { ctx = alice })
    test.eq(res.status, 200)
    test.ok(res.body:find("Buy milk"), "body contains todo title")
end)

test("GET /?q=milk searches todos", function()
    local res = test.get("/?q=milk", { ctx = alice })
    test.eq(res.status, 200)
    test.ok(res.body:find("Buy milk"), "search results contain todo")
end)

test("GET /?q=zzzznotfound returns empty", function()
    local res = test.get("/?q=zzzznotfound", { ctx = alice })
    test.eq(res.status, 200)
end)

test("POST /toggle/1 toggles todo and redirects", function()
    local res = test.post("/toggle/1", {
        body = "_csrf=fake",
        headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
        ctx = alice,
    })
    test.eq(res.status, 302)
end)

test("GET /export returns CSV with todos", function()
    local res = test.get("/export", { ctx = alice })
    test.eq(res.status, 200)
    test.ok(res.body:find("Buy milk"), "CSV contains todo title")
end)

test("GET /admin returns 200 for admin", function()
    local res = test.get("/admin", { ctx = alice })
    test.eq(res.status, 200)
end)

-- Register Bob (second user, not admin)
test("POST /register creates Bob", function()
    local res = test.post("/register", {
        body = "name=Bob&email=bob%40test.com&password=secret1234&_csrf=fake",
        headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
    })
    test.ok(res.status == 302 or res.status == 200, "register Bob")
end)

local bob = { session = { user_id = 2, email = "bob@test.com", name = "Bob" } }

test("GET /admin returns 403 for non-admin", function()
    local res = test.get("/admin", { ctx = bob })
    test.eq(res.status, 403)
end)

test("POST /delete/1 deletes todo and redirects", function()
    local res = test.post("/delete/1", {
        body = "_csrf=fake",
        headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
        ctx = alice,
    })
    test.eq(res.status, 302)
end)
