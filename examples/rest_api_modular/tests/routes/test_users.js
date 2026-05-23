// Hull's test.post takes an opts object where `body` is a raw string;
// routes/users.js decodes JSON manually, so the tests JSON-encode here.

test("POST /users creates a user", async () => {
    const res = await test.post("/users", {
        body: JSON.stringify({ email: "a@b.test", name: "Alice" }),
    });
    test.eq(res.status, 201);
    test.ok(res.json.id);
    test.eq(res.json.email, "a@b.test");
});

test("POST /users rejects missing email", async () => {
    const res = await test.post("/users", {
        body: JSON.stringify({ name: "no email" }),
    });
    test.eq(res.status, 400);
    test.ok(res.json.errors.email);
});

test("GET /users/:id returns 404 for unknown id", async () => {
    const res = await test.get("/users/does-not-exist");
    test.eq(res.status, 404);
});
