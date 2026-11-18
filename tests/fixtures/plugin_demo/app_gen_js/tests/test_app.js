// hull test runs the build pipeline first: inventory.json is generated.
test("the generated inventory is loaded", () => {
  const r = test.runMain([]);
  test.eq(r.code, 0);
  test.eq(r.stdout, "Inventory: bye: parting, hello: greeting (hello, bye)\n");
});
