// The sources a pipeline generation shadows (tests/e2e_generation.sh).
import { app } from "hull:app";
import { template } from "hull:template";
import { v } from "./lib/m.js";
app.manifest({ modules: ["hull/template@1"] });
app.main((ctx) => {
    ctx.stdout.write("entry=disk m=" + v + " page=" +
        template.render("page.html", {}) + "\n");
    return 0;
});
