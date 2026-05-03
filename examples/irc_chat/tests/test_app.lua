-- Tests for irc_chat example
-- Run: hull test examples/irc_chat/
--
-- Note: middleware (session loading) does not run during hull test dispatch.
-- Protected routes return 401. WebSocket endpoints cannot be tested here.

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

test("GET /ws/connections returns count", function()
    local res = test.get("/ws/connections")
    test.eq(res.status, 200)
    test.eq(res.json.count, 0)
end)

-- ── Registration ────────────────────────────────────────────────────

test("POST /register creates a user with keypair", function()
    local res = test.post("/register", {
        body = '{"username":"alice","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 201)
    test.eq(res.json.username, "alice")
    test.ok(res.json.public_key, "has public_key")
    test.ok(res.json.secret_key, "has secret_key")
    test.ok(res.json.id, "has id")
end)

test("POST /register rejects duplicate username", function()
    test.post("/register", {
        body = '{"username":"dupuser","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    local res = test.post("/register", {
        body = '{"username":"dupuser","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 409)
end)

test("POST /register rejects short password", function()
    local res = test.post("/register", {
        body = '{"username":"shortpw","password":"short"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

test("POST /register rejects short username", function()
    local res = test.post("/register", {
        body = '{"username":"a","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

-- ── Login ───────────────────────────────────────────────────────────

test("POST /login succeeds with correct credentials", function()
    test.post("/register", {
        body = '{"username":"loginuser","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    local res = test.post("/login", {
        body = '{"username":"loginuser","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 200)
    test.eq(res.json.username, "loginuser")
    test.ok(res.json.public_key, "has public_key")
end)

test("POST /login rejects wrong password", function()
    test.post("/register", {
        body = '{"username":"wrongpw","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    local res = test.post("/login", {
        body = '{"username":"wrongpw","password":"badpassword"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 401)
end)

test("POST /login rejects unknown user", function()
    local res = test.post("/login", {
        body = '{"username":"nobody","password":"secret1234"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 401)
end)

-- ── Channels ────────────────────────────────────────────────────────

test("GET /channels returns empty list", function()
    local res = test.get("/channels")
    test.eq(res.status, 200)
    test.ok(res.json.channels, "has channels")
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

test("POST /channels returns 401 without session", function()
    local res = test.post("/channels", {
        body = '{"name":"#test"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 401)
end)
