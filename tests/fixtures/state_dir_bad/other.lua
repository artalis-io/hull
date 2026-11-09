-- Refused at load: an "@other/" root is reserved.
app.manifest({ name = "state-dir-bad", modules = { "hull/fs@1" }, fs = { read = { "@other/" } } })
app.main(function() print("ran=yes") return 0 end)
