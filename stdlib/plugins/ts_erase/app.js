// stdlib:ts-erase@1 - the bundled TypeScript erasure plugin
// (docs/typescript_design.md §4.1). Hull runs it on the typescript route
// (docs/buildcontext_design.md §13.7): every analyzed .ts / .mts file of the
// app is one unit. It parses nothing: the host's frontend session, the one
// project discovery analyzed the file in, erases the types and answers the
// erased bytes, which keep every offset, line and column. A refused construct
// is an error diagnostic, and the action commits nothing.
//
// SPDX-License-Identifier: AGPL-3.0-or-later
import { app } from "hull:app";

app.manifest({
  modules: ["hull/build-plugin@1"],
  buildPlugin: {
    id: "ts-erase", version: 1, input: "source", languages: ["typescript"],
    output: "generated",
  },
});

app.transform(async (ctx) => {
  // Every unit's refusals are reported; once one is refused nothing more is
  // written, since the action will not commit.
  let refused = false;
  for (const unit of ctx.source.units()) {
    const r = ctx.source.erasedSource(unit);
    if (r.refusals.length) {
      for (const d of r.refusals) ctx.diagnostics.emit(d);
      refused = true;
    } else if (!refused) {
      const w = ctx.outputs.createBlob(unit.id);
      w.write(r.bytes);
      w.close();
      ctx.outputs.declareGeneratedOutput({ name: unit.name, position_map: "identity" }, w);
    }
  }
});
