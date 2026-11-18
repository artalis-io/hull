// A build plugin driven by the real host (stdlib/cli/lua/hull/buildctx/tests/
// test_plugin.lua); app.lua in build_host_lua is the same transform in Lua
// and must commit a byte-identical result.
import { app } from "hull:app";

app.manifest({
  modules: ["hull/build-plugin@1"],
  buildPlugin: {
    id: "demo", version: 1, input: "source", languages: ["lua", "javascript"],
    output: "generated", config: { greeting: { type: "string", required: true } },
  },
});

function refusedWith(code, fn) {
  try {
    fn();
  } catch (e) {
    if (e.code === code) return;
    throw e;
  }
  throw new Error("expected " + code);
}

app.transform(async (ctx) => {
  const mode = ctx.meta.mode;
  if (mode === "fail") throw new Error("boom");
  if (mode === "spin") for (;;) { /* until the host or the budget stops it */ }
  const src = ctx.inputs.read("src/a.txt");
  const st = ctx.inputs.stat("src/a.txt", { size: true });
  const list = ctx.inputs.list("src");
  const empty = ctx.inputs.list("empty");
  const units = ctx.source.units();
  const w = ctx.outputs.createBlob("out");
  w.write(ctx.config.greeting + ":");
  w.write(src);
  w.write(":" + st.size + ":" + list.length + ":" + empty.length + ":" + units.length);
  w.close();
  ctx.outputs.declareGeneratedOutput({ name: "./src/a.txt" }, w);
  const t = ctx.outputs.createTree("assets");
  t.write("x/y.txt", "y", "regular");
  t.close();
  ctx.outputs.declareArtifact({ name: "assets", kind: "demo-tree" }, t);
  ctx.diagnostics.emit({ severity: "warning", code: "demo.unit", message: "unit " + units[0] });
  refusedWith("invalid_path", () => ctx.inputs.read("../x"));
  refusedWith("not_found", () => ctx.inputs.read("src/missing.txt"));
  refusedWith("closed", () => w.write("late"));
  if (mode === "undeclared") ctx.outputs.createBlob("loose").close();
});
