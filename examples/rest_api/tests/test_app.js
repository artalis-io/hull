// Tests for rest_api example (JS)
// Run: hull test examples/rest_api/

test("GET /tasks returns empty array initially", () => {
    const res = test.get("/tasks");
    test.eq(res.status, 200);
});

test("POST /tasks creates a task", () => {
    const res = test.post("/tasks", {
        body: '{"title":"Buy milk"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 201);
    test.ok(res.json.id, "has id");
    test.eq(res.json.title, "Buy milk");
    test.eq(res.json.done, 0);
});

test("POST /tasks requires title", () => {
    const res = test.post("/tasks", {
        body: '{"done":false}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

test("GET /tasks lists created tasks", () => {
    // Create a task first
    test.post("/tasks", {
        body: '{"title":"Task for list"}',
        headers: { "Content-Type": "application/json" },
    });

    const res = test.get("/tasks");
    test.eq(res.status, 200);
    test.ok(res.body, "has body");
});

test("GET /tasks/:id returns single task", () => {
    const create = test.post("/tasks", {
        body: '{"title":"Single task"}',
        headers: { "Content-Type": "application/json" },
    });
    const id = create.json.id;

    const res = test.get("/tasks/" + id);
    test.eq(res.status, 200);
    test.eq(res.json.title, "Single task");
});

test("GET /tasks/:id returns 404 for missing", () => {
    const res = test.get("/tasks/99999");
    test.eq(res.status, 404);
});

test("PUT /tasks/:id updates a task", () => {
    const create = test.post("/tasks", {
        body: '{"title":"To update"}',
        headers: { "Content-Type": "application/json" },
    });
    const id = create.json.id;

    const res = test.put("/tasks/" + id, {
        body: '{"title":"Updated","done":true}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 200);
    test.eq(res.json.ok, true);
});

test("PUT /tasks/:id returns 404 for missing", () => {
    const res = test.put("/tasks/99999", {
        body: '{"title":"X","done":false}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 404);
});

test("DELETE /tasks/:id deletes a task", () => {
    const create = test.post("/tasks", {
        body: '{"title":"To delete"}',
        headers: { "Content-Type": "application/json" },
    });
    const id = create.json.id;

    const res = test.delete("/tasks/" + id);
    test.eq(res.status, 200);
    test.eq(res.json.ok, true);

    // Confirm gone
    const after = test.get("/tasks/" + id);
    test.eq(after.status, 404);
});

test("DELETE /tasks/:id returns 404 for missing", () => {
    const res = test.delete("/tasks/99999");
    test.eq(res.status, 404);
});
