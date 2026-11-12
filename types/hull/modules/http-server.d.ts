// hull:http-server - the inbound HTTP server's stats. Declaring `hull/http-server`
// also installs app.get / post / ... / use / router (see hull:app).
declare module "hull:http-server" {
    /** A snapshot of the server's connection counters. */
    export interface ServerStats {
        /** Connections open now. */
        activeConnections: number;
        /** The `--max-connections` ceiling. */
        maxConnections: number;
        /** Connections suspended on an async op. */
        asyncSuspended: number;
        /** Whether accepting is paused (at the ceiling). */
        listenPaused: boolean;
    }

    /** The inbound HTTP server. */
    export const httpServer: {
        /** The server's counters; throws when no server is running (e.g. in `app.main` only apps). */
        stats(): ServerStats;
    };
}
