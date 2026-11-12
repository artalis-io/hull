// hull:retry - retry-with-backoff for any fallible operation.
declare module "hull:retry" {
    /** Backoff options (`retry.backoff`, and `retry.run`'s). */
    export interface BackoffOptions {
        /** First delay in ms (default 100). */
        baseMs?: number;
        /** Growth per attempt, >= 1 (default 2). */
        factor?: number;
        /** Largest delay in ms (default 30000). */
        capMs?: number;
        /** Full jitter: a uniform delay in [0, delay). */
        jitter?: boolean;
    }
    /** Options for `retry.run`. */
    export interface RetryOptions<T> extends BackoffOptions {
        /** Total attempts including the first (default 3, at most 1000). */
        maxAttempts?: number;
        /** Retry when a success value is "bad". */
        retryOn?: (result: T) => boolean | Promise<boolean>;
        /** Return false to rethrow an error at once. */
        retryOnError?: (err: unknown) => boolean | Promise<boolean>;
        /** Called before each backoff sleep. */
        onRetry?: (attempt: number, ok: boolean, value: unknown) => void;
    }
    /** The `hull:retry` module object. */
    export interface RetryModule {
        /** The backoff delay (ms) for a 1-based attempt. */
        backoff(attempt: number, opts?: BackoffOptions): number;
        /** Run `fn`, retrying on a throw / rejection (never on a runtime refusal) or a `retryOn` result; resolves to the last result or rethrows the last error. */
        run<T>(fn: () => T | Promise<T>, opts?: RetryOptions<T>): Promise<T>;
    }
    /** Retry with backoff. */
    export const retry: RetryModule;
    /** Retry with backoff (default export). */
    const _default: RetryModule;
    export default _default;
}
