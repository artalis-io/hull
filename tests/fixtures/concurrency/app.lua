-- Concurrency-cap fixture (tests/e2e_concurrency.sh): slow handlers park on
-- hull.sleep so requests stay in flight. 127.0.0.1 is a trusted proxy, so the
-- test picks the client address with X-Forwarded-For.
app.manifest({
    modules = { "hull/http-server@1" },
    limits = {
        per_ip_concurrency = 8,
        trusted_proxies = { "127.0.0.1", "::1" },
        route_concurrency = { ["/narrow"] = 2 },
    },
})

app.get("/slow", function(_req, res)
    hull.sleep(1500)
    res:text("slow")
end)

app.get("/narrow", function(_req, res)
    hull.sleep(1500)
    res:text("narrow")
end)

app.get("/ping", function(_req, res)
    res:text("pong")
end)
