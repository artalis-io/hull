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

    /** Bytes a build plugin writes: text (its UTF-8 bytes) or a buffer. */
    export type BuildPluginBytes = string | ArrayBuffer | ArrayBufferView;

    /** A blob a build plugin writes (`ctx.outputs.createBlob`). */
    export interface BuildPluginBlob {
        write(bytes: BuildPluginBytes): true;
        close(): true;
    }

    /** A file tree a build plugin writes (`ctx.outputs.createTree`). */
    export interface BuildPluginTree {
        write(path: string, bytes: BuildPluginBytes, mode?: "regular" | "executable"): true;
        close(): true;
    }

    /** What `app.transform`'s function is given for one action. Every call is
     * one request to the build host; a refused call throws an Error whose
     * `code` is the host's token. Values are plain (no functions, no class
     * instances); bytes arrive as an ArrayBuffer. */
    /** A diagnostic a transform emits (`ctx.diagnostics.emit`). */
    export interface BuildPluginDiagnostic {
        severity: "error" | "warning" | "info";
        code: string;
        message: string;
        path?: string;
        range?: { start: number; stop: number; line?: number; col?: number };
    }

    /** One unit of the action's source selection: an annotated declaration, or a whole
     *  file on a Hull route (`kind: "module"`, with the analyzed bytes' `sha256`). */
    export interface BuildPluginUnit {
        id: string;
        language: string;
        path: string;
        kind: string;
        name: string;
        sha256?: string;
        range?: { start: number; stop: number; line: number; col: number };
        annotations?: { name: string; value?: unknown }[];
    }

    export interface BuildPluginContext {
        /** The action's metadata. */
        meta: Record<string, unknown>;
        /** The plugin's configuration (`hull.build.json`). */
        config: Record<string, unknown>;
        inputs: {
            read(path: string): ArrayBuffer;
            stat(path: string, opts?: { exec?: boolean }): unknown;
            list(path: string): unknown;
        };
        outputs: {
            createBlob(name: string): BuildPluginBlob;
            createTree(name: string): BuildPluginTree;
            declareGeneratedOutput(descriptor: { name: string; [k: string]: unknown },
                                   blob: BuildPluginBlob): true;
            declareArtifact(descriptor: { name: string; [k: string]: unknown },
                            tree: BuildPluginTree | BuildPluginBlob): true;
        };
        diagnostics: {
            emit(d: BuildPluginDiagnostic): true;
        };
        source: {
            units(): BuildPluginUnit[];
            /** The erased JavaScript of a TypeScript unit (or its id): `bytes` is absent
             *  when there is a refusal. Throws `unsupported_op` for another language. */
            erasedSource(unit: BuildPluginUnit | string): {
                bytes?: ArrayBuffer;
                refusals: BuildPluginDiagnostic[];
            };
        };
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
        /** Register a build plugin's transform, run once per action the build host
         * sends (docs/buildcontext_design.md §9.1). Requires `hull/build-plugin`. */
        transform(fn: (ctx: BuildPluginContext) => unknown): void;
    };
}
