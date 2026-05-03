// Tests for irc_chat example (JavaScript)
// Run: hull test examples/irc_chat/
//
// Note: middleware (session loading) does not run during hull test dispatch.
// Protected routes return 401. WebSocket endpoints cannot be tested here.

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

test("GET /ws/connections returns count", () => {
    const res = test.get("/ws/connections");
    test.eq(res.status, 200);
    test.eq(res.json.count, 0);
});

// ── Registration ────────────────────────────────────────────────────

test("POST /register creates a user with keypair", () => {
    const res = test.post("/register", {
        body: '{"username":"alice","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 201);
    test.eq(res.json.username, "alice");
    test.ok(res.json.public_key, "has public_key");
    test.ok(res.json.secret_key, "has secret_key");
});

test("POST /register rejects duplicate username", () => {
    test.post("/register", {
        body: '{"username":"dupuser","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    const res = test.post("/register", {
        body: '{"username":"dupuser","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 409);
});

test("POST /register rejects short password", () => {
    const res = test.post("/register", {
        body: '{"username":"shortpw","password":"short"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

test("POST /register rejects short username", () => {
    const res = test.post("/register", {
        body: '{"username":"a","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

// ── Login ───────────────────────────────────────────────────────────

test("POST /login succeeds with correct credentials", () => {
    test.post("/register", {
        body: '{"username":"loginuser","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    const res = test.post("/login", {
        body: '{"username":"loginuser","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 200);
    test.eq(res.json.username, "loginuser");
    test.ok(res.json.public_key, "has public_key");
});

test("POST /login rejects wrong password", () => {
    test.post("/register", {
        body: '{"username":"wrongpw","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    const res = test.post("/login", {
        body: '{"username":"wrongpw","password":"badpassword"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 401);
});

test("POST /login rejects unknown user", () => {
    const res = test.post("/login", {
        body: '{"username":"nobody","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 401);
});

// ── Channels ────────────────────────────────────────────────────────

test("GET /channels returns list", () => {
    const res = test.get("/channels");
    test.eq(res.status, 200);
    test.ok(res.json.channels, "has channels");
});

// ── Protected routes require auth ───────────────────────────────────

test("GET /me returns 401 without session", () => {
    const res = test.get("/me");
    test.eq(res.status, 401);
});

test("POST /logout returns 401 without session", () => {
    const res = test.post("/logout");
    test.eq(res.status, 401);
});

test("POST /channels returns 401 without session", () => {
    const res = test.post("/channels", {
        body: '{"name":"#test"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 401);
});
