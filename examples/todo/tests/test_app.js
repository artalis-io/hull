// Tests for todo example (with auth, search, csv, rbac) - JS
// Run: hull test examples/todo/
//
// Note: middleware registered via app.use() does not run during hull test
// dispatch. CSRF, session, and rate limiting are not active in these tests.
// Authenticated routes are tested by injecting session data via opts.ctx.

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

// ── CSV export ──────────────────────────────────────────────────────

test("GET /export redirects to /login without session", () => {
    const res = test.get("/export");
    test.eq(res.status, 302);
});

// ── Admin dashboard ─────────────────────────────────────────────────

test("GET /admin redirects to /login without session", () => {
    const res = test.get("/admin");
    test.eq(res.status, 302);
});

// ── Authenticated routes (inject session via ctx) ───────────────────

// Alice was registered above (user_id=1, first user → admin role)
const alice = { session: { user_id: 1, email: "alice@test.com", name: "Alice" } };

test("GET / returns 200 with session", () => {
    const res = test.get("/", { ctx: alice });
    test.eq(res.status, 200);
});

test("POST /add creates todo and redirects", () => {
    const res = test.post("/add", {
        body: "title=Buy+milk&_csrf=fake",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        ctx: alice,
    });
    test.eq(res.status, 302);
});

test("GET / shows added todo", () => {
    const res = test.get("/", { ctx: alice });
    test.eq(res.status, 200);
    test.ok(res.body.includes("Buy milk"), "body contains todo title");
});

test("GET /?q=milk searches todos", () => {
    const res = test.get("/?q=milk", { ctx: alice });
    test.eq(res.status, 200);
    test.ok(res.body.includes("Buy milk"), "search results contain todo");
});

test("GET /?q=zzzznotfound returns empty", () => {
    const res = test.get("/?q=zzzznotfound", { ctx: alice });
    test.eq(res.status, 200);
});

test("POST /toggle/1 toggles todo and redirects", () => {
    const res = test.post("/toggle/1", {
        body: "_csrf=fake",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        ctx: alice,
    });
    test.eq(res.status, 302);
});

test("GET /export returns CSV with todos", () => {
    const res = test.get("/export", { ctx: alice });
    test.eq(res.status, 200);
    test.ok(res.body.includes("Buy milk"), "CSV contains todo title");
});

test("GET /admin returns 200 for admin", () => {
    const res = test.get("/admin", { ctx: alice });
    test.eq(res.status, 200);
});

// Register Bob (second user, not admin)
test("POST /register creates Bob", () => {
    const res = test.post("/register", {
        body: "name=Bob&email=bob%40test.com&password=secret1234&_csrf=fake",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
    });
    test.ok(res.status === 302 || res.status === 200, "register Bob");
});

const bob = { session: { user_id: 2, email: "bob@test.com", name: "Bob" } };

test("GET /admin returns 403 for non-admin", () => {
    const res = test.get("/admin", { ctx: bob });
    test.eq(res.status, 403);
});

test("POST /delete/1 deletes todo and redirects", () => {
    const res = test.post("/delete/1", {
        body: "_csrf=fake",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        ctx: alice,
    });
    test.eq(res.status, 302);
});
