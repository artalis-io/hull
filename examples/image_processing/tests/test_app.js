// Tests for image_processing example (JS)
// Run: hull test examples/image_processing/

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

test("GET /create returns PNG metadata", () => {
    const res = test.get("/create");
    test.eq(res.status, 200);
    test.eq(res.json.ok, true);
    test.eq(res.json.width, 4);
    test.eq(res.json.height, 4);
    test.eq(res.json.format, "rgba8");
    test.ok(res.json.pngSize > 0, "PNG size should be positive");
    test.ok(res.json.pngHex, "should have hex data");
});

test("GET /info returns round-trip info", () => {
    const res = test.get("/info");
    test.eq(res.status, 200);
    test.eq(res.json.ok, true);
    test.eq(res.json.original.width, 4);
    test.eq(res.json.original.height, 4);
    test.eq(res.json.decoded.width, 4);
    test.eq(res.json.decoded.height, 4);
    test.eq(res.json.roundTripMatch, true);
});
