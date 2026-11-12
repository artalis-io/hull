// hull:web:middleware:health - liveness (/health) and readiness (/ready) endpoints.
declare module "hull:web:middleware:health" {
    /** A readiness check: return true / nothing for ok, false (or throw) for a failure. Must be synchronous. */
    export type HealthCheck = () => boolean | void;
    /** One check's result. */
    export interface HealthCheckResult {
        /** `"ok"` or `"fail"`. */
        status: "ok" | "fail";
        /** Time the check took, milliseconds. */
        latency_ms: number;
        /** The failure's text, when it failed. */
        error?: string;
    }
    /** What `health.runChecks()` returns. */
    export interface HealthReport {
        /** Results by check name (`db` is the built-in ping). */
        checks: { [name: string]: HealthCheckResult };
        /** Whether every check passed. */
        allOk: boolean;
    }
    /** The connection the readiness ping uses: a connection, or the `hull:db` module (its default connection). */
    export type HealthDb = Hull.DbConnection | { default(): Hull.DbConnection };
    /** Options for `health.middleware()`. */
    export interface HealthOptions {
        /** Liveness path (default `"/health"`). */
        pathHealth?: string;
        /** Readiness path (default `"/ready"`). */
        pathReady?: string;
        /** Include the DB ping in `/ready` (default true). */
        dbCheck?: boolean;
        /** Return error text, latencies and server stats from `/ready` (default false; enable only behind auth). */
        details?: boolean;
        /** The connection to ping (as `setDb`). */
        db?: HealthDb;
    }
    /** The `health` module object. */
    export interface HealthModule {
        /** Register a readiness check (replaces one of the same name; `"db"` is reserved and throws). */
        register(name: string, fn: HealthCheck): void;
        /** Remove a registered check. */
        unregister(name: string): void;
        /** Run the DB ping (unless `dbCheck: false`) and every registered check. */
        runChecks(opts?: { dbCheck?: boolean }): HealthReport;
        /** Build the middleware answering `/health` and `/ready` (503 when a check fails). */
        middleware(opts?: HealthOptions): Hull.Middleware;
        /** Choose the connection the readiness ping uses. */
        setDb(conn: HealthDb | null): void;
    }
    /** Liveness and readiness endpoints. */
    export const health: HealthModule;
}
