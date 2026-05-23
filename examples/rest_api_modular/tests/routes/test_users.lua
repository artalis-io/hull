-- Hull's test.post takes an opts table where `body` is a raw string;
-- routes/users.lua decodes JSON manually, so the tests json-encode here.
local json = require("hull.json")

test("POST /users creates a user", function()
    local res = test.post("/users", {
        body = json.encode({ email = "a@b.test", name = "Alice" }),
    })
    test.eq(res.status, 201)
    test.ok(res.json.id)
    test.eq(res.json.email, "a@b.test")
end)

test("POST /users rejects missing email", function()
    local res = test.post("/users", {
        body = json.encode({ name = "no email" }),
    })
    test.eq(res.status, 400)
    test.ok(res.json.errors.email)
end)

test("GET /users/:id returns 404 for unknown id", function()
    local res = test.get("/users/does-not-exist")
    test.eq(res.status, 404)
end)
