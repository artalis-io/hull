// The demonstration app in JavaScript; app_lua/app.lua is the same app.
import { app } from "hull:app";

app.manifest({});

/** @demo greeting */
function hello() { return "hello"; }

/** @demo parting */
function bye() { return "bye"; }

/** @other not selected */
function other() { return "other"; }

app.main(() => {
  console.log(hello(), bye(), other());
  return 0;
});
