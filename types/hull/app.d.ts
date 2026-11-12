// The `app` intrinsic (`hull:app`). Route, middleware, WebSocket, SSE and timer
// methods exist only when the manifest declares the module that installs them.
declare module "hull:app" {
    /** A route group under a path prefix (`app.router(prefix)`); every method is chainable. */
    export interface Router {
        /** The path prefix every route of the group starts with. */
        prefix: string;
        /** The options the router was made with (unused by Hull). */
        opts: unknown;
        /** Register a GET route under the prefix. */
        get(pattern: string, handler: Hull.Handler): Router;
        /** Register a POST route under the prefix. */
        post(pattern: string, handler: Hull.Handler): Router;
        /** Register a PUT route under the prefix. */
        put(pattern: string, handler: Hull.Handler): Router;
        /** Register a DELETE route under the prefix. */
        delete(pattern: string, handler: Hull.Handler): Router;
        /** Register a PATCH route under the prefix. */
        patch(pattern: string, handler: Hull.Handler): Router;
        /** Register an OPTIONS route under the prefix. */
        options(pattern: string, handler: Hull.Handler): Router;
        /** Middleware for every method under `prefix/*`. */
        use(middleware: Hull.Middleware): Router;
        /** Middleware for `method` on `prefix + pattern`. */
        use(method: string, pattern: string, middleware: Hull.Middleware): Router;
        /** Post-body middleware for every method under `prefix/*`. */
        usePost(middleware: Hull.Middleware): Router;
        /** Post-body middleware for `method` on `prefix + pattern`. */
        usePost(method: string, pattern: string, middleware: Hull.Middleware): Router;
        /** A WebSocket endpoint under the prefix (no-op without `hull/web/ws-server`). */
        ws(pattern: string, handlers: Hull.WsHandlers): Router;
        /** An SSE endpoint under the prefix (no-op without `hull/web/sse`). */
        sse(pattern: string, handler: Hull.SseHandler): Router;
        /** A nested group under `prefix + sub`. */
        router(sub?: string, opts?: unknown): Router;
    }

    /** Options of `app.daily`. */
    export interface DailyOptions {
        /** Fire at local time instead of UTC. */
        localtime?: boolean;
    }

    /** The application object. */
    export const app: {
        /** Register the app's entry point (once); see docs/cli_mode.md. */
        main(fn: Hull.MainFunction): void;
        /** Declare the app's capabilities (once, at the top level). */
        manifest(manifest: Hull.Manifest): void;
        /** The declared manifest (a frozen copy), or `null` before `app.manifest()`. */
        getManifest(): Hull.Manifest | null;
        /** Register a GET route. Requires `hull/http-server`. */
        get: Hull.RouteMethod;
        /** Register a POST route. Requires `hull/http-server`. */
        post: Hull.RouteMethod;
        /** Register a PUT route. Requires `hull/http-server`. */
        put: Hull.RouteMethod;
        /** Register a DELETE route. Requires `hull/http-server`. */
        delete: Hull.RouteMethod;
        /** Register a DELETE route (alias of `delete`). Requires `hull/http-server`. */
        del: Hull.RouteMethod;
        /** Register a PATCH route. Requires `hull/http-server`. */
        patch: Hull.RouteMethod;
        /** Register an OPTIONS route. Requires `hull/http-server`. */
        options: Hull.RouteMethod;
        /** Register middleware for `method` (`"*"` = any) on `pattern` (`"/*"` = prefix). Requires `hull/http-server`. */
        use(method: string, pattern: string, middleware: Hull.Middleware): void;
        /** Register middleware that runs after the handler. Requires `hull/http-server`. */
        usePost(method: string, pattern: string, middleware: Hull.Middleware): void;
        /** A route group under `prefix`. Requires `hull/http-server`. */
        router(prefix?: string, opts?: unknown): Router;
        /** Register a WebSocket endpoint. Requires `hull/web/ws-server`. */
        ws(path: string, handlers: Hull.WsHandlers): void;
        /** Register a Server-Sent Events endpoint. Requires `hull/web/sse`. */
        sse(path: string, handler: Hull.SseHandler): void;
        /** Run `fn` every `intervalMs` ms (at least 100). Requires `hull/timers`. */
        every(intervalMs: number, fn: () => unknown): void;
        /** Run `fn` daily at `"HH:MM"` (UTC unless `localtime`). Requires `hull/timers`. */
        daily(time: string, fn: () => unknown, opts?: DailyOptions): void;
    };
}
