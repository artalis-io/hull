// Concurrency-cap fixture (tests/e2e_concurrency.sh): slow handlers park on
// hull.sleep so requests stay in flight. 127.0.0.1 is a trusted proxy, so the
// test picks the client address with X-Forwarded-For.
import { app } from "hull:app";

app.manifest({
    modules: ["hull/http-server@1"],
    limits: {
        perIpConcurrency: 8,
        trustedProxies: ["127.0.0.1", "::1"],
        routeConcurrency: { "/narrow": 2 },
    },
});

app.get("/slow", async (_req, res) => {
    await hull.sleep(1500);
    res.text("slow");
});

app.get("/narrow", async (_req, res) => {
    await hull.sleep(1500);
    res.text("narrow");
});

app.get("/ping", (_req, res) => {
    res.text("pong");
});
