app.manifest({ modules = {"hull/http-server@1"} })
app.get("/", function(req, res) res:json({status = "ok"}) end)
