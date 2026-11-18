// A plugin on a Hull route (stdlib/cli/lua/hull/buildctx/tests/test_plan.lua
// runs it as a bundled plugin on a test route): it is shown whole source
// files, not declarations, and has no workspace grants - it writes each
// file's generated twin from the unit alone.
import { app } from "hull:app";

app.manifest({
  modules: ["hull/build-plugin@1"],
  buildPlugin: {
    id: "route-demo", version: 1, input: "source", languages: ["typescript"],
    output: "generated",
  },
});

app.transform(async (ctx) => {
  for (const u of ctx.source.units()) {
    if (u.kind !== "module") throw new Error("not a module unit: " + u.kind);
    let code = null;
    try { ctx.inputs.read(u.path); } catch (e) { code = e.code; }
    if (code !== "permission") throw new Error("a route stage read the workspace: " + code);
    const w = ctx.outputs.createBlob(u.id);
    w.write("// " + u.path + " " + u.sha256 + "\n");
    w.close();
    ctx.outputs.declareGeneratedOutput({ name: u.name, position_map: "identity" }, w);
  }
});
