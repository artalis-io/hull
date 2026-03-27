-- Tests for udf example
-- Run: hull test examples/udf/

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.ok, true)
end)

test("GET /products returns uppercased names", function()
    local res = test.get("/products")
    test.eq(res.status, 200)
    test.ok(res.json.products, "has products")
    test.eq(res.json.products[1].name, "APPLE")
    test.eq(res.json.products[2].name, "BANANA")
    test.eq(res.json.products[3].name, "CARROT")
end)

test("GET /products includes all fields", function()
    local res = test.get("/products")
    test.eq(res.status, 200)
    local p = res.json.products[1]
    test.ok(p.id, "has id")
    test.ok(p.name, "has name")
    test.ok(p.category, "has category")
    test.ok(p.price, "has price")
end)

test("GET /avg-prices returns averages per category", function()
    local res = test.get("/avg-prices")
    test.eq(res.status, 200)
    test.ok(res.json.averages, "has averages")

    -- Should have 3 categories: dairy, fruit, vegetable
    local avgs = res.json.averages
    test.eq(#avgs, 3)
end)

test("GET /avg-prices fruit average is correct", function()
    local res = test.get("/avg-prices")
    local avgs = res.json.averages
    -- Find fruit category (Apple 1.50 + Banana 0.75) / 2 = 1.125
    for _, row in ipairs(avgs) do
        if row.category == "fruit" then
            test.eq(row.avg_price, 1.125)
        end
    end
end)
