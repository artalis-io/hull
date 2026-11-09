-- Refused at load: a state.name Hull does not take.
app.manifest({ name = "state-dir-bad", state = { name = "../etc" }, modules = { "hull/fs@1" },
               fs = { write = { "@state/" } } })
app.main(function() print("ran=yes") return 0 end)
