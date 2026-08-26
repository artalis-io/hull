// routes/health.js - liveness + readiness endpoints.

export function register(app) {
    app.get("/health", (_req, res) => {
        res.json({ status: "ok" });
    });
}
