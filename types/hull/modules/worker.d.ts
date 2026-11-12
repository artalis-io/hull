// hull:worker - run a function on the worker pool in a fresh VM.
declare module "hull:worker" {
    /** A value that crosses into or out of a worker: flat scalars only. */
    export type WorkerScalar = null | boolean | number | string;
    /** The context a dispatched function receives: a flat object of scalars. */
    export type WorkerContext = { [key: string]: WorkerScalar };
    /** What a dispatched function may return (an array arrives as an index-keyed object). */
    export type WorkerReturn = WorkerScalar | { [key: string]: WorkerScalar } | void;
    /** How a dispatch reports a failure of the function (it resolves, it does not reject). */
    export interface WorkerError {
        /** The failure's message. */
        error: string;
    }

    /** Worker-pool dispatch. */
    export const worker: {
        /**
         * Serialize `fn` (it must not close over anything) and run it with `ctx` on a pool
         * thread; resolves with its result (`undefined` becomes `null`) or `{ error }`.
         * Rejects with a `busy` / `wait_refused` refusal.
         */
        dispatch<C extends WorkerContext, R extends WorkerReturn>(
            fn: (ctx: C) => R, ctx?: C): Promise<(R extends void | undefined ? null : R) | WorkerError>;
    };
}
