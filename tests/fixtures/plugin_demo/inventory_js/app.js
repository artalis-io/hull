// The inventory build plugin in JavaScript; inventory_lua/app.lua is the same
// plugin.
import { app } from "hull:app";

app.manifest({
  modules: ["hull/build-plugin@1"],
  buildPlugin: {
    id: "inventory", version: 1, input: "source",
    languages: ["lua", "javascript"], annotations: ["demo"],
    output: "demo-inventory",
    config: { title: { type: "string", required: true } },
  },
});

app.transform(async (ctx) => {
  const lines = [];
  for (const u of ctx.source.units()) {
    ctx.inputs.read(u.path);                 // the source it describes is a dependency
    for (const a of u.annotations) lines.push(u.name + ": " + (a.value ?? ""));
  }
  lines.sort();
  const w = ctx.outputs.createBlob("inventory");
  w.write(ctx.config.title + "\n" + lines.join("\n") + "\n");
  w.close();
  ctx.outputs.declareArtifact({ name: "inventory", kind: "demo-inventory" }, w);
  ctx.diagnostics.emit({ severity: "info", code: "inventory.count",
                         message: lines.length + " declarations" });
});
