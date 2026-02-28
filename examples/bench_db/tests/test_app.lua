-- Tests for bench_db example
-- Run: hull test examples/bench_db/

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

test("GET /read returns seeded rows", function()
    local res = test.get("/read")
    test.eq(res.status, 200)
    test.ok(res.body, "has body")
end)

test("POST /write inserts a row", function()
    local res = test.post("/write")
    test.eq(res.status, 200)
    test.ok(res.json.inserted, "has inserted count")
end)

test("POST /write-batch inserts 10 rows", function()
    local res = test.post("/write-batch")
    test.eq(res.status, 200)
    test.eq(res.json.inserted, 10)
end)

test("GET /mixed does write + read", function()
    local res = test.get("/mixed")
    test.eq(res.status, 200)
    test.ok(res.body, "has body")
end)
