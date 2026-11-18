-- A build plugin declaring a module the preset refuses (time: a plugin's
-- output may not depend on the clock).
app.manifest({ modules = { "hull/build-plugin@1", "hull/time@1" } })
app.transform(function() end)
