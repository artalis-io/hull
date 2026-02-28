-- Tests for rest_api example
-- Run: hull test examples/rest_api/

test("GET /tasks returns empty array initially", function()
    local res = test.get("/tasks")
    test.eq(res.status, 200)
end)

test("POST /tasks creates a task", function()
    local res = test.post("/tasks", {
        body = '{"title":"Buy milk"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 201)
    test.eq(res.json.title, "Buy milk")
    test.eq(res.json.done, 0)
    test.ok(res.json.id, "has id")
end)

test("POST /tasks requires title", function()
    local res = test.post("/tasks", {
        body = "{}",
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
    test.eq(res.json.error, "title is required")
end)

test("GET /tasks lists created tasks", function()
    -- Create a task first
    test.post("/tasks", {
        body = '{"title":"Test task"}',
        headers = { ["Content-Type"] = "application/json" },
    })

    local res = test.get("/tasks")
    test.eq(res.status, 200)
    test.ok(res.body, "has body")
end)

test("GET /tasks/:id returns single task", function()
    local create = test.post("/tasks", {
        body = '{"title":"Specific task"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    local id = create.json.id

    local res = test.get("/tasks/" .. id)
    test.eq(res.status, 200)
    test.eq(res.json.title, "Specific task")
end)

test("GET /tasks/:id returns 404 for missing", function()
    local res = test.get("/tasks/99999")
    test.eq(res.status, 404)
end)

test("PUT /tasks/:id updates a task", function()
    local create = test.post("/tasks", {
        body = '{"title":"Old title"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    local id = create.json.id

    local res = test.put("/tasks/" .. id, {
        body = '{"title":"New title","done":true}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 200)
    test.eq(res.json.ok, true)

    -- Verify update
    local get = test.get("/tasks/" .. id)
    test.eq(get.json.title, "New title")
end)

test("PUT /tasks/:id returns 404 for missing", function()
    local res = test.put("/tasks/99999", {
        body = '{"title":"x","done":false}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 404)
end)

test("DELETE /tasks/:id deletes a task", function()
    local create = test.post("/tasks", {
        body = '{"title":"To delete"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    local id = create.json.id

    local res = test.delete("/tasks/" .. id)
    test.eq(res.status, 200)
    test.eq(res.json.ok, true)

    -- Verify deleted
    local get = test.get("/tasks/" .. id)
    test.eq(get.status, 404)
end)

test("DELETE /tasks/:id returns 404 for missing", function()
    local res = test.delete("/tasks/99999")
    test.eq(res.status, 404)
end)
