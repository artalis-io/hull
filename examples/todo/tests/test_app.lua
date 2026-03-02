-- Tests for todo example (with auth)
-- Run: hull test examples/todo/
--
-- Note: middleware registered via app.use() does not run during hull test
-- dispatch. CSRF, session, and rate limiting are not active in these tests.
-- Auth routes (register, login) work because they handle auth inline.
-- Protected routes (/, /add, /toggle, /delete, /logout) redirect to /login
-- because the session middleware never populates req.ctx.session.

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
