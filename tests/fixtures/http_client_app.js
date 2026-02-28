// http_client_app.js — E2E test app for HTTP client capability
//
// Declares hosts manifest to enable the http module,
// then exercises all HTTP methods against an echo server.
//
// Expects echo server running on 127.0.0.1:19860
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { app } from "hull:app";
import { log } from "hull:log";
import { http } from "hull:http";

app.manifest({
    hosts: ["127.0.0.1"],
    env: ["ECHO_PORT"],
});

const ECHO_BASE = "http://127.0.0.1:19860";

// GET /test/get — exercise http.get()
app.get("/test/get", (_req, res) => {
    const r = http.get(`${ECHO_BASE}/echo`);
    res.json({ status: r.status, echo: r.body });
});

// GET /test/post — exercise http.post()
app.get("/test/post", (_req, res) => {
    const r = http.post(`${ECHO_BASE}/echo`, "hello from js", {
        headers: { "X-Test": "js-post" }
    });
    res.json({ status: r.status, echo: r.body });
});

// GET /test/put — exercise http.put()
app.get("/test/put", (_req, res) => {
    const r = http.put(`${ECHO_BASE}/echo`, "put-body");
    res.json({ status: r.status, echo: r.body });
});

// GET /test/patch — exercise http.patch()
app.get("/test/patch", (_req, res) => {
    const r = http.patch(`${ECHO_BASE}/echo`, "patch-body");
    res.json({ status: r.status, echo: r.body });
});

// GET /test/delete — exercise http.delete()
app.get("/test/delete", (_req, res) => {
    const r = http.del(`${ECHO_BASE}/echo`);
    res.json({ status: r.status, echo: r.body });
});

// GET /test/request — exercise http.request() with custom method
app.get("/test/request", (_req, res) => {
    const r = http.request("OPTIONS", `${ECHO_BASE}/echo`);
    res.json({ status: r.status, echo: r.body });
});

// GET /test/headers — verify custom headers are sent
app.get("/test/headers", (_req, res) => {
    const r = http.get(`${ECHO_BASE}/echo`, {
        headers: { "X-Custom-Header": "test-value-js" }
    });
    res.json({ status: r.status, echo: r.body });
});

// GET /test/denied — verify host not in allowlist is rejected
app.get("/test/denied", (_req, res) => {
    try {
        http.get("http://evil.example.com/steal");
        res.json({ error: "should have been denied" });
    } catch (e) {
        res.json({ denied: true, message: e.message });
    }
});

// Health check for readiness
app.get("/health", (_req, res) => {
    res.json({ status: "ok", runtime: "quickjs" });
});

log.info("HTTP client test app loaded (JS)");
