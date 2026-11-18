// The demonstration app that loads a generated file (tests/e2e_generation.sh):
// inventory.json is not in the tree; the inventory plugin, configured with
// `module`, generates it. app_gen_lua/app.lua is the same app.
import { app } from "hull:app";
import inv from "./inventory.json";

app.manifest({});

/** @demo greeting */
function hello() { return "hello"; }

/** @demo parting */
function bye() { return "bye"; }

app.main((ctx) => {
  ctx.stdout.write(inv.title + ": " + inv.items.join(", ") + " (" + hello() + ", " + bye() + ")\n");
  return 0;
});
