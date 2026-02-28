-- Tests for auth example
-- Run: hull test examples/auth/
--
-- Note: middleware (session loading) does not run during hull test dispatch.
-- Protected routes (/me, /logout) return 401 because the session middleware
-- never populates req.ctx.session. We test registration, login, and validation.

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
    test.ok(res.json.id, "has id")
end)

test("POST /register rejects duplicate email", function()
    -- Register first
    test.post("/register", {
        body = '{"email":"dup@test.com","password":"secret1234","name":"Dup"}',
        headers = { ["Content-Type"] = "application/json" },
    })

    -- Try again
    local res = test.post("/register", {
        body = '{"email":"dup@test.com","password":"secret1234","name":"Dup"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 409)
end)

test("POST /register rejects short password", function()
    local res = test.post("/register", {
        body = '{"email":"short@test.com","password":"short","name":"Short"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
    test.ok(res.json.error, "has error message")
end)

test("POST /register rejects missing email", function()
    local res = test.post("/register", {
        body = '{"password":"secret1234","name":"NoEmail"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

test("POST /register rejects missing name", function()
    local res = test.post("/register", {
        body = '{"email":"noname@test.com","password":"secret1234"}',
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
    test.eq(res.json.name, "Login")
end)

test("POST /login rejects wrong password", function()
    test.post("/register", {
        body = '{"email":"badpw@test.com","password":"secret1234","name":"BadPw"}',
        headers = { ["Content-Type"] = "application/json" },
    })

    local res = test.post("/login", {
        body = '{"email":"badpw@test.com","password":"wrongpassword"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 401)
end)

test("POST /login rejects unknown email", function()
    local res = test.post("/login", {
        body = '{"email":"nobody@test.com","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 401)
end)

-- ── Protected routes (middleware doesn't run in test mode) ──────────

test("GET /me returns 401 without session", function()
    local res = test.get("/me")
    test.eq(res.status, 401)
end)

test("POST /logout returns 401 without session", function()
    local res = test.post("/logout")
    test.eq(res.status, 401)
end)
