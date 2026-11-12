// Globals app code sees: `hull` and `console`.

declare namespace Hull {
    /** Options of `hull.map`. */
    interface MapOptions {
        /** Most items in flight at once (default 16; `Infinity` for no cap). */
        limit?: number;
    }

    /** The error `hull.map` rejects with: the first failure's message, every failure attached. */
    interface MapError extends Error {
        /** The failures by item index (sparse). */
        errors: unknown[];
    }
}

/** Hull's runtime helpers. */
declare var hull: {
    /** Resolve after `ms` milliseconds without blocking the event loop. */
    sleep(ms: number): Promise<void>;
    /** Run `fn` over `items`, at most `opts.limit` at once; results in item order. Rejects with a `Hull.MapError` once every item finished. */
    map<T, R>(items: readonly T[], fn: (item: T, index: number) => R | Promise<R>, opts?: Hull.MapOptions): Promise<R[]>;
};

/** Logging to Hull's log, tagged `[app]` (each argument converted to a string). */
declare var console: {
    /** Log at INFO. */
    log(...data: unknown[]): void;
    /** Log at INFO. */
    info(...data: unknown[]): void;
    /** Log at WARN. */
    warn(...data: unknown[]): void;
    /** Log at ERROR. */
    error(...data: unknown[]): void;
};
