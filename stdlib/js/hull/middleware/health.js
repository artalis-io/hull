/*
 * hull:middleware:health -- Health check endpoints
 *
 * Provides /health (liveness) and /ready (readiness) endpoints with
 * built-in DB ping, custom check registration, server stats, and uptime.
 *
 * Usage:
 *   import { health } from "hull:middleware:health";
 *   health.register("cache", () => true);
 *   app.use("GET", "/*", health.middleware());
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { db } from "hull:db";
import { time } from "hull:time";
import { server } from "hull:server";

const _checks = {};
let _startTime = null;

function register(name, fn) {
    _checks[name] = fn;
}

function unregister(name) {
    delete _checks[name];
}

function runChecks(opts) {
    const o = opts || {};
    const results = {};
    let allOk = true;

    // DB check
    if (o.dbCheck !== false && db) {
        const t0 = time.clock();
        try {
            db.query("SELECT 1");
            const latency = Math.round((time.clock() - t0) * 1000 * 10) / 10;
            results.db = { status: "ok", latency_ms: latency };
        } catch (e) {
            const latency = Math.round((time.clock() - t0) * 1000 * 10) / 10;
            results.db = { status: "fail", error: String(e), latency_ms: latency };
            allOk = false;
        }
    }

    // Custom checks
    for (const name in _checks) {
        const t0 = time.clock();
        try {
            const result = _checks[name]();
            const latency = Math.round((time.clock() - t0) * 1000 * 10) / 10;
            if (result === false) {
                results[name] = { status: "fail", error: "check returned false", latency_ms: latency };
                allOk = false;
            } else {
                results[name] = { status: "ok", latency_ms: latency };
            }
        } catch (e) {
            const latency = Math.round((time.clock() - t0) * 1000 * 10) / 10;
            results[name] = { status: "fail", error: String(e), latency_ms: latency };
            allOk = false;
        }
    }

    return { checks: results, allOk };
}

function middleware(opts) {
    const o = opts || {};
    const pathHealth = o.pathHealth || "/health";
    const pathReady = o.pathReady || "/ready";
    const doDbCheck = o.dbCheck !== false;

    if (_startTime === null) {
        _startTime = time.now();
    }

    return function healthMiddleware(req, res) {
        if (req.method !== "GET" && req.method !== "HEAD") {
            return 0;
        }

        const uptime = Math.floor(time.now() - _startTime);

        if (req.path === pathHealth) {
            res.json({ status: "ok", uptime });
            return 1;
        }

        if (req.path === pathReady) {
            const result = runChecks({ dbCheck: doDbCheck });

            const body = {
                status: result.allOk ? "ok" : "fail",
                checks: result.checks,
                uptime,
            };

            if (server && server.stats) {
                body.stats = server.stats();
            }

            if (result.allOk) {
                res.json(body);
            } else {
                res.status(503).json(body);
            }
            return 1;
        }

        return 0;
    };
}

const health = { register, unregister, runChecks, middleware };
export { health };
