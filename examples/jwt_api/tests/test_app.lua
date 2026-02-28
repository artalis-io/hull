-- Tests for jwt_api example
-- Run: hull test examples/jwt_api/
--
-- Note: middleware (JWT extraction) does not run during hull test dispatch.
-- Token-protected routes (/me, /refresh) return 401 because the JWT middleware
-- never populates req.ctx.user. We test registration, login, and validation.

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

test("POST /register rejects short password", function()
    local res = test.post("/register", {
        body = '{"email":"short@test.com","password":"short","name":"Short"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

test("POST /register rejects missing fields", function()
    local res = test.post("/register", {
        body = '{"email":"x@test.com","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

-- ── Login ───────────────────────────────────────────────────────────

test("POST /login returns JWT token", function()
    test.post("/register", {
        body = '{"email":"jwt@test.com","password":"secret1234","name":"JWT"}',
        headers = { ["Content-Type"] = "application/json" },
    })

    local res = test.post("/login", {
        body = '{"email":"jwt@test.com","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 200)
    test.ok(res.json.token, "has token")
    test.ok(res.json.user, "has user")
    test.eq(res.json.user.email, "jwt@test.com")
    test.eq(res.json.user.name, "JWT")
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

test("POST /login rejects missing fields", function()
    local res = test.post("/login", {
        body = '{"email":"jwt@test.com"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

-- ── Protected routes (middleware doesn't run in test mode) ──────────

test("GET /me returns 401 without token", function()
    local res = test.get("/me")
    test.eq(res.status, 401)
end)

test("POST /refresh returns 401 without token", function()
    local res = test.post("/refresh")
    test.eq(res.status, 401)
end)
