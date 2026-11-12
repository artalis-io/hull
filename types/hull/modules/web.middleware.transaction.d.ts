// hull:web:middleware:transaction - wrap handler DB work in a transaction.
declare namespace Hull {
    interface RequestContext {
        /** True when `transaction.middleware()` marked the request for transactional handling. */
        _txn?: boolean;
    }
}

declare module "hull:web:middleware:transaction" {
    /** The `transaction` module object. */
    export interface TransactionModule {
        /** Marker middleware: sets `req.ctx._txn = true` and continues. */
        middleware(opts?: Record<string, never>): Hull.Middleware;
        /** Run `fn` in a transaction on the default connection (`db.batch`); rethrows after rollback. fn must be synchronous. */
        run(fn: () => void): void;
        /** Run `fn` in a transaction; returns `[true, null]` or `[false, error]` after rollback. */
        attempt(fn: () => void): [true, null] | [false, unknown];
        /** Alias of `attempt` (Lua's `transaction.try`). */
        try(fn: () => void): [true, null] | [false, unknown];
    }
    /** Transaction helpers over the default connection. */
    export const transaction: TransactionModule;
}
