-- Connection-cap fixture (tests/e2e_concurrency.sh): at most four open
-- connections per client address, idle keep-alive ones included. No trusted
-- proxies, so the TCP peer is the client.
app.manifest({
    modules = { "hull/http-server@1" },
    limits = {
        per_ip_connections = 4,
    },
})

app.get("/ping", function(_req, res)
    res:text("pong")
end)
