-- The sources a pipeline generation shadows (tests/e2e_generation.sh).
app.manifest({ modules = { "hull/template@1" } })
local template = require("hull.template")
local m = require("./lib/m")
app.main(function(ctx)
    ctx.stdout:write("entry=disk m=" .. m.v .. " page=" ..
        template.render("page.html", {}) .. "\n")
    return 0
end)
