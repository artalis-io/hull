// hull:web:middleware:cors - CORS middleware factory.
declare module "hull:web:middleware:cors" {
    /** Options for `cors.middleware()`. */
    export interface CorsOptions {
        /** Allowed origins; `"*"` allows any (default `["*"]`). Not with `credentials: true`. */
        origins?: string[];
        /** `Access-Control-Allow-Methods` on preflight (default `"GET, POST, PUT, DELETE, OPTIONS"`). */
        methods?: string;
        /** `Access-Control-Allow-Headers` on preflight (default `"Content-Type, Authorization"`). */
        headers?: string;
        /** Send `Access-Control-Allow-Credentials: true` (default false). */
        credentials?: boolean;
        /** `Access-Control-Max-Age` in seconds (default 86400). */
        maxAge?: number;
        /** Lua-parity alias of `maxAge`. */
        max_age?: number;
    }
    /** The `cors` module object. */
    export interface CorsModule {
        /** Build a CORS middleware; answers an OPTIONS preflight with 204. Throws when `credentials` is combined with `"*"`. */
        middleware(opts?: CorsOptions): Hull.Middleware;
        /** Whether `origin` is in `origins` (`"*"` matches any). */
        isAllowedOrigin(origin: string | null | undefined, origins: readonly string[] | null | undefined): boolean;
    }
    /** CORS middleware factory. */
    export const cors: CorsModule;
}
