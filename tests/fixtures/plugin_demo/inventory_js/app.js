// The inventory build plugin in JavaScript; inventory_lua/app.lua is the same
// plugin.
import { app } from "hull:app";

app.manifest({
  modules: ["hull/build-plugin@1"],
  buildPlugin: {
    id: "inventory", version: 1, input: "source",
    languages: ["lua", "javascript"], annotations: ["demo"],
    output: "demo-inventory",
    config: { title: { type: "string", required: true },
              module: { type: "string" } },
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
  // With `module` set, the inventory is also an app file the app loads: a
  // generated output, which `hull dev` and an app run from source are handed.
  if (ctx.config.module) {
    const m = ctx.outputs.createBlob("module");
    m.write('{"items":[' + lines.map((l) => JSON.stringify(l)).join(",") + '],"title":' +
            JSON.stringify(ctx.config.title) + "}\n");
    m.close();
    ctx.outputs.declareGeneratedOutput({ name: ctx.config.module, position_map: "none" }, m);
  }
  ctx.diagnostics.emit({ severity: "info", code: "inventory.count",
                         message: lines.length + " declarations" });
});
