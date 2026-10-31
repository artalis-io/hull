-- Concurrency-cap fixture with NO trusted proxies: X-Forwarded-For is the
-- client's own text and must not pick the key (tests/e2e_concurrency.sh).
app.manifest({ modules = { "hull/http-server@1" } })

app.get("/slow", function(_req, res)
    hull.sleep(1500)
    res:text("slow")
end)

app.get("/ping", function(_req, res)
    res:text("pong")
end)
