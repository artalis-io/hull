// hull:web:middleware:logger - request logging middleware (logfmt).
declare namespace Hull {
    interface RequestContext {
        /** The request id (also sent as `X-Request-ID`); set by `logger.middleware()`. */
        request_id?: string;
    }
}

declare module "hull:web:middleware:logger" {
    /** Options for `logger.middleware()`. */
    export interface LoggerOptions {
        /** Paths not logged (exact match). */
        skip?: string[];
        /** Request header names whose values are added to the line. */
        includeHeaders?: string[];
        /** Lua-parity alias of `includeHeaders`. */
        include_headers?: string[];
    }
    /** The `logger` module object. */
    export interface LoggerModule {
        /** A fresh request id (8+ hex characters, a per-process counter). */
        generateId(): string;
        /** Format `[key, value]` pairs as one logfmt line. */
        formatLine(entries: ReadonlyArray<readonly [string, string | number | boolean | null | undefined]>): string;
        /** Whether `path` is in `skipList` (exact match). */
        shouldSkip(path: string, skipList: readonly string[] | null | undefined): boolean;
        /** Build the logger middleware: one `log.info` line per request, sets `req.ctx.request_id`. */
        middleware(opts?: LoggerOptions): Hull.Middleware;
    }
    /** Request logging middleware. */
    export const logger: LoggerModule;
}
