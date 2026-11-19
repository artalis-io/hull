// stdlib:ts-erase@1 against a stub host (test.runTransform): the erased
// bytes the host's frontend answers become one generated output per unit,
// named for the file it shadows; a refusal is an error diagnostic and
// nothing is written. Run by tests/e2e_typescript.sh.

const unit = (path) => ({ id: "module:typescript:" + path, language: "typescript", path,
                          kind: "module", name: "./" + path, sha256: "0".repeat(64) });
const UNITS = [unit("app.ts"), unit("lib/a.ts")];
// An output's bytes as text (the sources here are ASCII).
const text = (o) => String.fromCharCode(...new Uint8Array(o.bytes));

test("each unit's erased bytes are its generated output", async () => {
  const r = await test.runTransform({
    units: UNITS,
    erased: { "module:typescript:app.ts": "const x         = 1;\n",
              "module:typescript:lib/a.ts": "export const y = 2;\n" },
  });
  test.eq(r.error, undefined);
  test.eq(r.diagnostics.length, 0);
  test.eq(r.outputs.length, 2);
  test.eq(r.outputs[0].declared, "generated");
  test.eq(r.outputs[0].descriptor.name, "./app.ts");
  test.eq(r.outputs[0].descriptor.position_map, "identity");
  test.eq(text(r.outputs[0]), "const x         = 1;\n");
  test.eq(r.outputs[1].descriptor.name, "./lib/a.ts");
});

test("a refusal is reported and nothing is written", async () => {
  const refusal = { severity: "error", code: "ts.unsupported", message: "TypeScript enums are not supported",
                    path: "lib/a.ts", range: { start: 1, stop: 5 } };
  const r = await test.runTransform({
    units: UNITS,
    erased: { "module:typescript:app.ts": { refusals: [refusal] },
              "module:typescript:lib/a.ts": "export const y = 2;\n" },
  });
  test.eq(r.diagnostics.length, 1);
  test.eq(r.diagnostics[0].code, "ts.unsupported");
  test.eq(r.diagnostics[0].path, "lib/a.ts");
  test.eq(r.outputs.length, 0);
});

test("no units, no outputs", async () => {
  const r = await test.runTransform({ units: [] });
  test.eq(r.error, undefined);
  test.eq(r.outputs.length, 0);
});
