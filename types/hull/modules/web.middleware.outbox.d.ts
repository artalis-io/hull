// hull:web:middleware:outbox - transactional outbox for reliable side effects.
declare namespace Hull {
    interface RequestContext {
        /** True when `outbox.middleware()` marked the request for `outbox.flushIfNeeded(req)`. */
        _outbox_flush?: boolean;
    }
}

declare module "hull:web:middleware:outbox" {
    /** What `outbox.enqueue()` takes. */
    export interface OutboxItem {
        /** The delivery kind, e.g. `"webhook"` or `"http"`. */
        kind: string;
        /** Target URL or address. */
        destination: string;
        /** Body to deliver. */
        payload: string;
        /** JSON-encoded headers object. */
        headers?: string;
        /** Dedup key (at most 255 bytes of UTF-8, no NUL). */
        idempotencyKey?: string;
        /** Max delivery attempts for this item (default: the module's). */
        maxAttempts?: number;
        /** Seconds before the first attempt (default 0). */
        delay?: number;
    }
    /** What `outbox.flush()` reports. */
    export interface OutboxFlushResult {
        /** Items delivered. */
        delivered: number;
        /** Items that reached their max attempts and were marked failed. */
        failed: number;
        /** Items rescheduled after a failed attempt. */
        retried: number;
        /** Set when the runtime refused a delivery as busy; the rest stay pending. */
        busy?: true;
    }
    /** Row counts per state. */
    export interface OutboxStats {
        /** Rows waiting for delivery. */
        pending: number;
        /** Rows delivered. */
        delivered: number;
        /** Rows that exhausted their attempts. */
        failed: number;
    }
    /** The `outbox` module object. */
    export interface OutboxModule {
        /** Create the `_hull_outbox` table (idempotent); `maxAttempts` sets the default (5). */
        init(opts?: { maxAttempts?: number }): void;
        /** Queue a side effect; call inside `db.batch` to commit it with the state change. Returns the row id. */
        enqueue(item: OutboxItem): number;
        /** Deliver due items (at most `limit`, default 50); refuses inside a transaction. */
        flush(opts?: { limit?: number }): Promise<OutboxFlushResult>;
        /** Marker middleware: sets `req.ctx._outbox_flush = true`. */
        middleware(): Hull.Middleware;
        /** Flush when `outbox.middleware()` marked the request. */
        flushIfNeeded(req: Hull.Request): Promise<void>;
        /** Count rows per state. */
        stats(): OutboxStats;
        /** Delete delivered rows older than `maxAge` seconds (default 7 days); `alsoFailed` deletes old failed rows too. Returns the count. */
        cleanup(maxAge?: number | null, opts?: { alsoFailed?: boolean }): number;
    }
    /** Transactional outbox. */
    export const outbox: OutboxModule;
}
