// A build plugin (tests/e2e_build_plugin.sh); app.lua in build_plugin_lua is
// the same transform in Lua and must drive the host identically.
import { app } from "hull:app";

app.manifest({ modules: ["hull/build-plugin@1"] });

app.transform(async (ctx) => {
  const src = ctx.inputs.read("src/a.txt");
  const w = ctx.outputs.createBlob("out");
  w.write(ctx.config.greeting + ":");
  w.write(src);
  w.close();
  ctx.outputs.declareGeneratedOutput({ name: "./a.txt" }, w);
  ctx.diagnostics.emit({ severity: "info", code: "demo.unit", message: "unit " + ctx.meta.unit });
  try {
    ctx.inputs.read("../escape");
    throw new Error("a denied read must throw");
  } catch (e) {
    if (e.code !== "denied") throw e;
  }
  if (ctx.meta.unit === "b") throw new Error("unit b fails");
});
