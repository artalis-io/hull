// hull:log - the app's log (each argument converted to a string, tagged [app]).
declare module "hull:log" {
    /** Logging to Hull's log. */
    export const log: {
        /** Log at INFO. */
        info(...args: unknown[]): void;
        /** Log at WARN. */
        warn(...args: unknown[]): void;
        /** Log at ERROR. */
        error(...args: unknown[]): void;
        /** Log at DEBUG. */
        debug(...args: unknown[]): void;
    };
}
