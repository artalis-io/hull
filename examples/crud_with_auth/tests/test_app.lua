-- Tests for crud_with_auth example
-- Run: hull test examples/crud_with_auth/
--
-- Note: middleware (session loading) does not run during hull test dispatch.
-- All task CRUD routes and /me, /logout return 401 because the session
-- middleware never populates req.ctx.session. We test auth endpoints
-- and verify that task routes require authentication.

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

-- ── Registration ────────────────────────────────────────────────────

test("POST /register creates a user", function()
    local res = test.post("/register", {
        body = '{"email":"alice@test.com","password":"secret1234","name":"Alice"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 201)
    test.eq(res.json.email, "alice@test.com")
    test.eq(res.json.name, "Alice")
end)

test("POST /register rejects duplicate email", function()
    test.post("/register", {
        body = '{"email":"dup@test.com","password":"secret1234","name":"Dup"}',
        headers = { ["Content-Type"] = "application/json" },
    })

    local res = test.post("/register", {
        body = '{"email":"dup@test.com","password":"secret1234","name":"Dup"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 409)
end)

test("POST /register validates input", function()
    local res = test.post("/register", {
        body = '{"email":"","password":"secret1234","name":"X"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

-- ── Login ───────────────────────────────────────────────────────────

test("POST /login succeeds with correct credentials", function()
    test.post("/register", {
        body = '{"email":"login@test.com","password":"secret1234","name":"Login"}',
        headers = { ["Content-Type"] = "application/json" },
    })

    local res = test.post("/login", {
        body = '{"email":"login@test.com","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 200)
    test.eq(res.json.email, "login@test.com")
end)

test("POST /login rejects wrong password", function()
    test.post("/register", {
        body = '{"email":"badpw@test.com","password":"secret1234","name":"BadPw"}',
        headers = { ["Content-Type"] = "application/json" },
    })

    local res = test.post("/login", {
        body = '{"email":"badpw@test.com","password":"wrong"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 401)
end)

-- ── Protected routes require auth ───────────────────────────────────

test("GET /me returns 401 without session", function()
    local res = test.get("/me")
    test.eq(res.status, 401)
end)

test("POST /logout returns 401 without session", function()
    local res = test.post("/logout")
    test.eq(res.status, 401)
end)

test("GET /tasks returns 401 without session", function()
    local res = test.get("/tasks")
    test.eq(res.status, 401)
end)

test("POST /tasks returns 401 without session", function()
    local res = test.post("/tasks", {
        body = '{"title":"Test"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 401)
end)

test("GET /tasks/:id returns 401 without session", function()
    local res = test.get("/tasks/1")
    test.eq(res.status, 401)
end)

test("PUT /tasks/:id returns 401 without session", function()
    local res = test.put("/tasks/1", {
        body = '{"title":"Updated","done":true}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 401)
end)

test("DELETE /tasks/:id returns 401 without session", function()
    local res = test.delete("/tasks/1")
    test.eq(res.status, 401)
end)
