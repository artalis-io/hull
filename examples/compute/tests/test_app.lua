-- Tests for compute example

test("GET /health", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
end)

test("GET /echo returns echoed text", function()
    local res = test.get("/echo?text=hello")
    test.eq(res.status, 200)
    test.eq(res.json.input, "hello")
    test.eq(res.json.output, "hello")
    test.eq(res.json.match, true)
end)

test("GET /score returns a score", function()
    local res = test.get("/score?text=hello")
    test.eq(res.status, 200)
    test.eq(res.json.text, "hello")
    assert(type(res.json.score) == "number", "expected number score")
    assert(res.json.score >= 0 and res.json.score <= 100, "score out of range")
end)
