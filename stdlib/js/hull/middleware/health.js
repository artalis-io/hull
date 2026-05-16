/**
 * @file hull:middleware:health
 * @module hull:middleware:health
 * @description Liveness + readiness HTTP endpoints. Lua parity:
 *   `hull.middleware.health`.
 *
 * `health.middleware()` intercepts `GET /health` and `GET /ready`:
 *
 *   - `/health` (liveness) — always returns `200 {status:"ok", uptime}`.
 *     Verifies the process is alive; never fails.
 *   - `/ready`  (readiness) — runs the DB ping (if enabled) and every
 *     check registered via `health.register()`. Returns `200` when all
 *     pass, otherwise `503` with a per-check breakdown.
 *
 * ES modules can't conditionally import — pass the `db` module
 * explicitly via `health.setDb(dbModule)` or `opts.db`.
 *
 * @license AGPL-3.0-or-later
 */

import { time } from "hull:time";
import { server } from "hull:server";

/* db module is optional — not all apps use a database. */
let _dbMod = null;

const _checks = {};
let _startTime = null;

/**
 * Register a custom readiness check.
 *
 * Replaces any prior check with the same name. Contract:
 *   - return `true` or no return value → ok
 *   - return `false` → fail
 *   - throw → fail (uses error string)
 *
 * @param {string}      name  Identifier (appears in `/ready` JSON output).
 * @param {() => boolean|void} fn
 */
function register(name, fn) {
    _checks[name] = fn;
}

/**
 * Remove a previously registered check.
 * @param {string} name
 */
function unregister(name) {
    delete _checks[name];
}

/**
 * Run the DB ping (if enabled) + every registered check.
 *
 * @param {Object} [opts]
 * @param {boolean} [opts.dbCheck=true]  Include the DB ping.
 * @returns {{ checks: Object<string,{status:string,latency_ms:number,error?:string}>,
 *             allOk: boolean }}
 */
function runChecks(opts) {
    const o = opts || {};
    const results = {};
    let allOk = true;

    // DB check (db module may not be available if app has no database)
    if (o.dbCheck !== false && _dbMod) {
        const t0 = time.clock();
        try {
            _dbMod.query("SELECT 1");
            const latency = Math.round((time.clock() - t0) * 10) / 10;
            results.db = { status: "ok", latency_ms: latency };
        } catch (e) {
            const latency = Math.round((time.clock() - t0) * 10) / 10;
            results.db = { status: "fail", error: String(e), latency_ms: latency };
            allOk = false;
        }
    }

    // Custom checks
    for (const name in _checks) {
        const t0 = time.clock();
        try {
            const result = _checks[name]();
            const latency = Math.round((time.clock() - t0) * 10) / 10;
            if (result === false) {
                results[name] = { status: "fail", error: "check returned false", latency_ms: latency };
                allOk = false;
            } else {
                results[name] = { status: "ok", latency_ms: latency };
            }
        } catch (e) {
            const latency = Math.round((time.clock() - t0) * 10) / 10;
            results[name] = { status: "fail", error: String(e), latency_ms: latency };
            allOk = false;
        }
    }

    return { checks: results, allOk };
}

/**
 * Inject the db module used for the readiness DB ping.
 *
 * Equivalent to passing `opts.db` to `middleware()`.
 *
 * @param {Object} dbModule  The `hull:db` module value.
 */
function setDb(dbModule) {
    _dbMod = dbModule;
}

/**
 * Build the `/health` + `/ready` middleware.
 *
 * Returns `1` (short-circuit) when the request hits one of the health
 * paths and `0` (continue) otherwise.
 *
 * @param {Object} [opts]
 * @param {string} [opts.pathHealth="/health"]
 * @param {string} [opts.pathReady="/ready"]
 * @param {boolean}[opts.dbCheck=true]  Include DB ping in `/ready`.
 * @param {Object} [opts.db]  Inject the db module (alternative to `setDb`).
 * @returns {(req, res) => number}
 */
function middleware(opts) {
    const o = opts || {};
    const pathHealth = o.pathHealth || "/health";
    const pathReady = o.pathReady || "/ready";
    const doDbCheck = o.dbCheck !== false;

    /* Accept db module via opts for apps that have a database */
    if (o.db) _dbMod = o.db;

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

const health = { register, unregister, runChecks, middleware, setDb };
export { health };
