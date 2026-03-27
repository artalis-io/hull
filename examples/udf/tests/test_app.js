// Tests for udf example (JS)
// Run: hull test examples/udf/

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.ok, true);
});

test("GET /products returns uppercased names", () => {
    const res = test.get("/products");
    test.eq(res.status, 200);
    test.ok(res.json.products, "has products");
    test.eq(res.json.products[0].name, "APPLE");
    test.eq(res.json.products[1].name, "BANANA");
    test.eq(res.json.products[2].name, "CARROT");
});

test("GET /products includes all fields", () => {
    const res = test.get("/products");
    test.eq(res.status, 200);
    const p = res.json.products[0];
    test.ok(p.id, "has id");
    test.ok(p.name, "has name");
    test.ok(p.category, "has category");
    test.ok(p.price, "has price");
});

test("GET /avg-prices returns averages per category", () => {
    const res = test.get("/avg-prices");
    test.eq(res.status, 200);
    test.ok(res.json.averages, "has averages");

    // Should have 3 categories: dairy, fruit, vegetable
    const avgs = res.json.averages;
    test.eq(avgs.length, 3);
});

test("GET /avg-prices fruit average is correct", () => {
    const res = test.get("/avg-prices");
    const avgs = res.json.averages;
    // Find fruit category (Apple 1.50 + Banana 0.75) / 2 = 1.125
    for (const row of avgs) {
        if (row.category === "fruit") {
            test.eq(row.avg_price, 1.125);
        }
    }
});
