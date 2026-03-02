// Tests for todo example (with auth) — JS
// Run: hull test examples/todo/
//
// Note: middleware registered via app.use() does not run during hull test
// dispatch. CSRF, session, and rate limiting are not active in these tests.

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

// ── Auth pages ──────────────────────────────────────────────────────

test("GET /login returns 200", () => {
    const res = test.get("/login");
    test.eq(res.status, 200);
});

test("GET /register returns 200", () => {
    const res = test.get("/register");
    test.eq(res.status, 200);
});

// ── Registration ────────────────────────────────────────────────────

test("POST /register creates user and redirects", () => {
    const res = test.post("/register", {
        body: "name=Alice&email=alice%40test.com&password=secret1234&_csrf=fake",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
    });
    test.ok(res.status === 302 || res.status === 200, "register response");
});

// ── Protected routes redirect without session ───────────────────────

test("GET / redirects to /login without session", () => {
    const res = test.get("/");
    test.eq(res.status, 302);
});

test("POST /add redirects to /login without session", () => {
    const res = test.post("/add", {
        body: "title=Test&_csrf=fake",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
    });
    test.eq(res.status, 302);
});

test("POST /toggle/1 redirects to /login without session", () => {
    const res = test.post("/toggle/1", {
        body: "_csrf=fake",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
    });
    test.eq(res.status, 302);
});

test("POST /delete/1 redirects to /login without session", () => {
    const res = test.post("/delete/1", {
        body: "_csrf=fake",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
    });
    test.eq(res.status, 302);
});
