// Tests for jwt_api example (JS)
// Run: hull test examples/jwt_api/
//
// Note: middleware (JWT extraction) does not run during hull test dispatch.
// Protected routes return 401 because the middleware never populates
// req.ctx.user. We test auth endpoints and verify protection.

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

// ── Registration ────────────────────────────────────────────────────

test("POST /register creates a user", () => {
    const res = test.post("/register", {
        body: '{"email":"alice@test.com","password":"secret1234","name":"Alice"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 201);
    test.eq(res.json.email, "alice@test.com");
    test.eq(res.json.name, "Alice");
});

test("POST /register rejects duplicate email", () => {
    test.post("/register", {
        body: '{"email":"dup@test.com","password":"secret1234","name":"Dup"}',
        headers: { "Content-Type": "application/json" },
    });

    const res = test.post("/register", {
        body: '{"email":"dup@test.com","password":"secret1234","name":"Dup"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 409);
});

test("POST /register rejects short password", () => {
    const res = test.post("/register", {
        body: '{"email":"short@test.com","password":"123","name":"Short"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

test("POST /register rejects missing fields", () => {
    const res = test.post("/register", {
        body: '{"email":""}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

// ── Login ───────────────────────────────────────────────────────────

test("POST /login returns JWT token", () => {
    test.post("/register", {
        body: '{"email":"login@test.com","password":"secret1234","name":"Login"}',
        headers: { "Content-Type": "application/json" },
    });

    const res = test.post("/login", {
        body: '{"email":"login@test.com","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 200);
    test.ok(res.json.token, "has token");
    test.ok(res.json.user, "has user");
});

test("POST /login rejects wrong password", () => {
    test.post("/register", {
        body: '{"email":"badpw@test.com","password":"secret1234","name":"BadPw"}',
        headers: { "Content-Type": "application/json" },
    });

    const res = test.post("/login", {
        body: '{"email":"badpw@test.com","password":"wrong"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 401);
});

test("POST /login rejects unknown email", () => {
    const res = test.post("/login", {
        body: '{"email":"nobody@test.com","password":"secret1234"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 401);
});

test("POST /login rejects missing fields", () => {
    const res = test.post("/login", {
        body: '{"email":""}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

// ── Protected routes require token ──────────────────────────────────

test("GET /me returns 401 without token", () => {
    const res = test.get("/me");
    test.eq(res.status, 401);
});

test("POST /refresh returns 401 without token", () => {
    const res = test.post("/refresh");
    test.eq(res.status, 401);
});
