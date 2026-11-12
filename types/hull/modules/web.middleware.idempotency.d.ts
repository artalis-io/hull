// hull:web:middleware:idempotency - Idempotency-Key replay for safe retries.
declare module "hull:web:middleware:idempotency" {
    /** Options for `idempotency.middleware()`. */
    export interface IdempotencyOptions {
        /** The caller's scope (a string or number). Default: the session / JWT user, else `"__anon"`; a falsy return skips the layer. */
        getPrincipal?: (req: Hull.Request) => string | number | bigint | null | undefined | false;
        /** Key lifetime for this instance, seconds (default: the module's). */
        ttl?: number;
        /** Request header carrying the key (default `"idempotency-key"`). */
        headerName?: string;
        /** Methods it applies to (default `["POST"]`). */
        methods?: string[];
    }
    /** Response headers to send (and cache, credential headers dropped). */
    export type IdempotencyHeaders = { [name: string]: string };
    /** The `idempotency` module object. */
    export interface IdempotencyModule {
        /** Create the `_hull_idempotency_keys` table (idempotent); `ttl` sets the default lifetime (86400). */
        init(opts?: { ttl?: number }): void;
        /** Build the middleware (register with `app.usePost`): replays a stored response, 409 on reuse with another body. */
        middleware(opts?: IdempotencyOptions): Hull.Middleware;
        /** Send a JSON response and store it for replay. */
        respond(req: Hull.Request, res: Hull.Response, statusCode: number, data: unknown, extraHeaders?: IdempotencyHeaders): void;
        /** Send an HTML response and store it for replay. */
        respondHtml(req: Hull.Request, res: Hull.Response, statusCode: number, html: string, extraHeaders?: IdempotencyHeaders): void;
        /** Mark the request's key complete without storing a response (retries re-run the handler). */
        complete(req: Hull.Request): void;
        /** Delete expired keys; returns the count. */
        cleanup(): number;
    }
    /** Idempotency-Key handling. */
    export const idempotency: IdempotencyModule;
}
