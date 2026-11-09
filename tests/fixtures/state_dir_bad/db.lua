-- Refused at load: a "@state/" database DSN with no state = { name } declared.
local db = require("hull.db")
app.manifest({ name = "state-dir-bad", modules = { "hull/db@1" },
               databases = { named = { default = "@state/x.db" } } })
app.main(function() print("ran=yes") return 0 end)
