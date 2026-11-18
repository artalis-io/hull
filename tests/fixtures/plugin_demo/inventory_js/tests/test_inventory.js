// The inventory transform against a stub host (test.runTransform): the same
// ctx calls, answered from this spec instead of a build host.

const UNITS = [
  { id: "lua:app.lua:function:hello:1-2", language: "lua", path: "app.lua", kind: "function",
    name: "hello", annotations: [{ name: "demo", value: "greeting" }] },
  { id: "lua:app.lua:function:bye:3-4", language: "lua", path: "app.lua", kind: "function",
    name: "bye", annotations: [{ name: "demo", value: "parting" }] },
];

test("the inventory lists the units it is shown", async () => {
  const r = await test.runTransform({
    config: { title: "Inventory" }, units: UNITS, inputs: { "app.lua": "-- app\n" },
  });
  test.eq(r.error, undefined);
  test.eq(r.outputs.length, 1);
  test.eq(r.outputs[0].declared, "artifact");
  test.eq(r.outputs[0].descriptor.kind, "demo-inventory");
  test.eq(r.outputs[0].text, "Inventory\nbye: parting\nhello: greeting\n");
  test.eq(r.outputs[0].bytes.byteLength, 39);
  test.eq(r.diagnostics.length, 1);
  test.eq(r.diagnostics[0].code, "inventory.count");
});

test("a unit whose source cannot be read fails the transform", async () => {
  const r = await test.runTransform({ config: { title: "X" }, units: UNITS, inputs: {} });
  test.ok(r.error && r.error.includes("not_found"), String(r.error));
  test.eq(r.outputs.length, 0);
});
