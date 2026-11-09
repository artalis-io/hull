-- Refused at load: a "@state/" grant with no state = { name } declared.
app.manifest({ name = "state-dir-bad", modules = { "hull/fs@1" }, fs = { write = { "@state/" } } })
app.main(function() print("ran=yes") return 0 end)
