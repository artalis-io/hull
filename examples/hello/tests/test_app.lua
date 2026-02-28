-- Tests for hello example
-- Run: hull test examples/hello/

test("GET / returns message", function()
    local res = test.get("/")
    test.eq(res.status, 200)
    test.ok(res.json.message, "response has message")
end)

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

test("GET /visits returns array", function()
    local res = test.get("/visits")
    test.eq(res.status, 200)
    test.ok(res.body, "response has body")
end)

test("POST /echo returns body", function()
    local res = test.post("/echo", { body = "hello world" })
    test.eq(res.status, 200)
    test.ok(res.json.body, "response has body field")
end)

test("GET /greet/:name returns greeting", function()
    local res = test.get("/greet/World")
    test.eq(res.status, 200)
    test.eq(res.json.greeting, "Hello, World!")
end)

test("POST /greet/:name returns greeting with body", function()
    local res = test.post("/greet/Hull", { body = "payload" })
    test.eq(res.status, 200)
    test.eq(res.json.greeting, "Hello, Hull!")
end)
