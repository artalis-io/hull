test("count: prints the number of items", () => {
    const r = test.runMain(["count", "a", "b", "c"]);
    test.eq(r.code, 0);
    test.eq(r.stdout, "3\n");
});
