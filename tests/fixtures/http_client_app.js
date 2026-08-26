// http_client_app.js - E2E test app for HTTP client capability
//
// Declares hosts manifest to enable the http module,
// then exercises all HTTP methods against an echo server.
//
// Expects echo server running on 127.0.0.1:19860
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { app } from "hull:app";
import { log } from "hull:log";
import { httpClient } from "hull:http-client";

app.manifest({
    modules: ["hull/http-client@1", "hull/http-server@1", "hull/log@1"],
    hosts: ["127.0.0.1"],
    env: ["ECHO_PORT"],
});

const ECHO_BASE = "http://127.0.0.1:19860";

// GET /test/get - exercise httpClient.get()
app.get("/test/get", (_req, res) => {
    const r = httpClient.get(`${ECHO_BASE}/echo`);
    res.json({ status: r.status, echo: r.body });
});

// GET /test/post - exercise httpClient.post()
app.get("/test/post", (_req, res) => {
    const r = httpClient.post(`${ECHO_BASE}/echo`, "hello from js", {
        headers: { "X-Test": "js-post" }
    });
    res.json({ status: r.status, echo: r.body });
});

// GET /test/put - exercise httpClient.put()
app.get("/test/put", (_req, res) => {
    const r = httpClient.put(`${ECHO_BASE}/echo`, "put-body");
    res.json({ status: r.status, echo: r.body });
});

// GET /test/patch - exercise http.patch()
app.get("/test/patch", (_req, res) => {
    const r = httpClient.patch(`${ECHO_BASE}/echo`, "patch-body");
    res.json({ status: r.status, echo: r.body });
});

// GET /test/delete - exercise httpClient.delete()
app.get("/test/delete", (_req, res) => {
    const r = httpClient.delete(`${ECHO_BASE}/echo`);
    res.json({ status: r.status, echo: r.body });
});

// GET /test/request - exercise httpClient.request() with custom method
app.get("/test/request", (_req, res) => {
    const r = httpClient.request("OPTIONS", `${ECHO_BASE}/echo`);
    res.json({ status: r.status, echo: r.body });
});

// GET /test/headers - verify custom headers are sent
app.get("/test/headers", (_req, res) => {
    const r = httpClient.get(`${ECHO_BASE}/echo`, {
        headers: { "X-Custom-Header": "test-value-js" }
    });
    res.json({ status: r.status, echo: r.body });
});

// GET /test/denied - verify host not in allowlist is rejected
app.get("/test/denied", (_req, res) => {
    try {
        httpClient.get("http://evil.example.com/steal");
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
