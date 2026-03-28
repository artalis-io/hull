// Tests for gpu_texture example (JS)
// Run: hull test examples/gpu_texture/
// Note: GPU tests only pass on machines with a GPU adapter

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.ok, true);
});
