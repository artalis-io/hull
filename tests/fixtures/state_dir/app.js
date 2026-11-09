import { app } from "hull:app";
import { fs } from "hull:fs";
// The JS twin of app.lua: the "@state/" root (shared/state_dir.h, gap 3).
app.manifest({
    name: "state-dir-e2e",
    state: { name: "hull-e2e-state" },
    modules: ["hull/fs@1"],
    fs: { read: ["@state/"], write: ["@state/keys/", "@state/notes.txt"] },
});
const text = (buf) => String.fromCharCode(...new Uint8Array(buf));
const fails = (fn) => { try { fn(); return false; } catch (e) { return true; } };
app.main(async (ctx) => {
    const mode = ctx.args[0] ?? "roundtrip";
    if (mode === "traverse") {
        console.log("traverse=" + String(fails(() => fs.read("@state/../outside.txt"))));
        console.log("other_root=" + String(fails(() => fs.read("@other/x"))));
        console.log("state_hull=" + String(fails(() => fs.write("@state/.hull/x", "no"))));
        return 0;
    }
    fs.write("@state/keys/trust.key", "secret-bytes", { private: true });
    fs.write("@state/notes.txt", "hello state");
    console.log("read_back=" + text(fs.read("@state/keys/trust.key")));
    console.log("notes=" + text(fs.read("@state/notes.txt")));
    const st = fs.stat("@state/keys/trust.key");
    console.log("stat_type=" + String(st && st.type));
    console.log("listed=" + fs.list("@state").map((e) => e.name).sort().join(","));
    console.log("app_root_denied=" + String(fails(() => fs.read("keys/trust.key"))));
    return 0;
});
