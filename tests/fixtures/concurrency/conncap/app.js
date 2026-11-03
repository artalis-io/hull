// Connection-cap fixture (tests/e2e_concurrency.sh): at most four open
// connections per client address, idle keep-alive ones included. No trusted
// proxies, so the TCP peer is the client.
import { app } from "hull:app";

app.manifest({
    modules: ["hull/http-server@1"],
    limits: {
        perIpConnections: 4,
    },
});

app.get("/ping", (_req, res) => {
    res.text("pong");
});
