// Shared shapes of Hull's JavaScript API. Merged across files (declare namespace).
declare namespace Hull {
    /** Any JSON value. */
    type Json = null | boolean | number | string | Json[] | { [key: string]: Json };
    /** A value whose shape the app defines - a database row field, a JSON body or payload, a job input, a context slot. Typed `any`, as `JSON.parse`'s result is; where the API takes a type parameter, pass one to type it. */
    type Data = any;
    /** A database row whose columns the app's schema defines: column name -> value. Pass a row type to `conn.query<R>()` to type it. */
    type DataRow = { [column: string]: Data };
    /** Bytes a binding accepts: an ArrayBuffer, a typed array view, or a Hull buffer. */
    type Bytes = ArrayBuffer | ArrayBufferView | WasmBuffer | MappedBuffer;

    // ── Requests and responses ───────────────────────────────────────────

    /**
     * Per-request context carried from middleware to the handler (`req.ctx`).
     * Stdlib middleware stores its results here (`session`, `user`, `csrf_token`, ...);
     * a module that adds a field may declare it by merging into this interface.
     */
    interface RequestContext { [key: string]: Data }

    /** An inbound HTTP request (route handlers and middleware). */
    interface Request {
        /** HTTP method, upper case (`"GET"`, `"POST"`, ...). */
        method: string;
        /** Request path without the query string (`"/invoices/42"`). */
        path: string;
        /** Route parameters by name (`/users/:id` -> `params.id`); a null-prototype object. */
        params: Record<string, string>;
        /** Query-string parameters, URL-decoded (form rules); a null-prototype object. */
        query: Record<string, string>;
        /** Request headers by lower-case name; a null-prototype object. */
        headers: Record<string, string>;
        /**
         * The body decoded as UTF-8 text (invalid sequences become U+FFFD), `""` for an
         * empty body, `null` when the request carries no buffered body (a GET, a
         * streaming-multipart route, an SSE endpoint).
         */
        body: string | null;
        /** The body's exact bytes; present only when the body is not valid UTF-8. */
        bodyBytes?: ArrayBuffer;
        /** The client's numeric IP address from the socket (IPv4-mapped shown as IPv4); absent when unknown. */
        remote_addr?: string;
        /** Per-request context object shared by the middleware chain and the handler. */
        ctx: RequestContext;
        /** Case-insensitive header lookup; `undefined` when the header was not sent. */
        header(name: string): string | undefined;
        /**
         * Iterate a streaming `multipart/form-data` body. Present only on routes
         * registered with `{ multipart: {...} }` (see docs/app_api_reference.md
         * "Streaming Multipart Uploads").
         */
        multipart?: () => MultipartIterator;
    }

    /** The request of a streaming-multipart route: `multipart()` is always there. */
    interface MultipartRequest extends Request {
        /** Iterate the streaming `multipart/form-data` body (the route has no `body`). */
        multipart: () => MultipartIterator;
    }

    /** One part of a streaming multipart body; invalid once the iterator steps past it. */
    interface MultipartPart {
        /** The part's form-field name (`""` when the part has none). */
        readonly name: string;
        /** The uploaded file's name, or `null` for a plain form field. */
        readonly filename: string | null;
        /** The part's Content-Type, or `null` when it sent none. */
        readonly contentType: string | null;
        /** Read the whole part into memory (refused when larger than the VM heap limit). */
        read(): Promise<ArrayBuffer>;
        /** Iterate the part's bytes chunk by chunk; `minBytes` is an advisory hint. */
        chunks(minBytes?: number): MultipartChunks;
    }

    /** The async iterator `req.multipart()` returns: one `MultipartPart` per step. */
    interface MultipartIterator extends AsyncIterator<MultipartPart, undefined> {
        /** The next part, or `{ done: true }` at the end of the body. */
        next(): Promise<IteratorResult<MultipartPart, undefined>>;
        /** The iterator itself (`for await (const part of req.multipart())`). */
        [Symbol.asyncIterator](): MultipartIterator;
    }

    /** The async iterator `part.chunks()` returns: one ArrayBuffer per step. */
    interface MultipartChunks extends AsyncIterator<ArrayBuffer, undefined> {
        /** The next chunk, or `{ done: true }` at the end of the part. */
        next(): Promise<IteratorResult<ArrayBuffer, undefined>>;
        /** The iterator itself (`for await (const chunk of part.chunks())`). */
        [Symbol.asyncIterator](): MultipartChunks;
    }

    /** The response a handler or middleware builds. */
    interface Response {
        /** Set the status code; chainable. */
        status(code: number): this;
        /** Add a header (name must be a token, value may not hold NUL / CR / LF); chainable. */
        header(name: string, value: string | number): this;
        /** Send `data` as JSON (`JSON.stringify`), optionally with a status code. */
        json(data: unknown, status?: number): void;
        /** Send an HTML body (`text/html; charset=utf-8`, plus the manifest's CSP). */
        html(body: string): void;
        /** Send a plain-text body (`text/plain; charset=utf-8`). */
        text(body: string): void;
        /** Send raw bytes as the body; no Content-Type is set and nothing is compressed. */
        bytes(body: string | Bytes): void;
        /** Redirect to `url` (status 302 unless given). */
        redirect(url: string, status?: number): void;
    }

    /** A route handler; a returned Promise is awaited before the response is sent. */
    type Handler = (req: Request, res: Response) => unknown;
    /**
     * A middleware: return 0 (or nothing) to continue, 1 when it answered the request.
     * Middleware is synchronous: a returned Promise answers 500.
     */
    type Middleware = (req: Request, res: Response) => number | void;

    /** Options of a route registration (`app.get(pattern, handler, opts)`). */
    interface RouteOptions {
        /** Make the route a streaming-multipart route (`req.multipart()`); caps in bytes / counts. */
        multipart?: MultipartCaps;
    }

    /** Options of a streaming-multipart route (`app.post(pattern, handler, { multipart })`). */
    interface MultipartRouteOptions extends RouteOptions {
        /** The route's multipart caps (`{}` takes every default). */
        multipart: MultipartCaps;
    }

    /** A streaming-multipart route handler. */
    type MultipartHandler = (req: MultipartRequest, res: Response) => unknown;

    /** A route registration: `(pattern, handler, opts?)`; with `opts.multipart` the handler gets `req.multipart()`. */
    interface RouteMethod {
        /** Register a streaming-multipart route. */
        (pattern: string, handler: MultipartHandler, opts: MultipartRouteOptions): void;
        /** Register a route. */
        (pattern: string, handler: Handler, opts?: RouteOptions): void;
    }

    /** Caps of a streaming-multipart route; unset or 0 takes Hull's default (never unlimited). */
    interface MultipartCaps {
        /** Largest single part, in bytes. */
        maxPartSize?: number;
        /** Largest whole body, in bytes. */
        maxTotalSize?: number;
        /** Most parts in one body. */
        maxParts?: number;
        /** Largest header block of one part, in bytes. */
        maxHeadersSize?: number;
        /** Largest parser input buffer, in bytes. */
        maxInputBuffer?: number;
    }

    // ── WebSocket and SSE endpoints ──────────────────────────────────────

    /** A server-side WebSocket connection (`app.ws` callbacks). */
    interface WsConnection {
        /** Monotonic connection id (throws once the connection is closed). */
        readonly id: number;
        /** The endpoint path the connection was accepted on. */
        readonly path: string;
        /** Per-connection storage: an object created on first read, or whatever was assigned. */
        data: Data;
        /** Send a text frame; throws when the connection is closed. */
        send(text: string): true;
        /** Send a binary frame; throws when the connection is closed. */
        sendBinary(data: string | Bytes): true;
        /** Start the close handshake (default code 1000). */
        close(code?: number, reason?: string): void;
        /** Send a ping frame. */
        ping(data?: string): void;
    }

    /** The callbacks of a WebSocket endpoint (`app.ws(path, handlers)`). */
    interface WsHandlers {
        /** Called once the connection is upgraded. */
        onOpen?(conn: WsConnection): unknown;
        /** Called per message: a string for a text frame, an ArrayBuffer for a binary one. */
        onMessage?(conn: WsConnection, message: string | ArrayBuffer, isBinary: boolean): unknown;
        /** Called when the connection closes; `reason` is `null` when none was sent. */
        onClose?(conn: WsConnection, code: number, reason: string | null): unknown;
    }

    /** An outbound WebSocket client connection (`wsClient.connect`). */
    interface WsClientConnection {
        /** Send a text frame; throws when the connection is closed. */
        send(text: string): true;
        /** Send a binary frame; throws when the connection is closed. */
        sendBinary(data: string | Bytes): true;
        /** Start the close handshake (bounded by a 5 s close deadline). */
        close(code?: number, reason?: string): void;
        /** Send a ping frame. */
        ping(data?: string): void;
    }

    /** The callbacks of an outbound WebSocket connection (`wsClient.connect(url, handlers)`). */
    interface WsClientHandlers {
        /** Called once the upgrade completes. */
        onOpen?(conn: WsClientConnection): unknown;
        /** Called per message: a string for a text frame, an ArrayBuffer for a binary one. */
        onMessage?(conn: WsClientConnection, message: string | ArrayBuffer, isBinary: boolean): unknown;
        /** Called once when the connection closes; `reason` is `null` when none was sent. */
        onClose?(conn: WsClientConnection, code: number, reason: string | null): unknown;
        /** Called on a connect, handshake or protocol failure. */
        onError?(conn: WsClientConnection, error: string): unknown;
    }

    /** A Server-Sent Events stream (`app.sse` handlers). */
    interface SseStream {
        /** Send an event (`name` null / undefined omits the event type; `data` is split on newlines). */
        event(name: string | null | undefined, data: string, id?: string | null): void;
        /** Send a comment line (a keep-alive). */
        comment(text: string): void;
        /** End the stream. */
        close(): void;
    }

    /** An SSE endpoint handler: runs for the life of the stream. */
    type SseHandler = (req: Request, stream: SseStream) => unknown;

    // ── Manifest ─────────────────────────────────────────────────────────

    /** The first-party module names a manifest may declare (`hull modules available`). */
    type ModuleName =
        | "hull/app" | "hull/archive/tar" | "hull/attachment" | "hull/blob" | "hull/cache"
        | "hull/compute" | "hull/config" | "hull/crypto" | "hull/crypto/envelope"
        | "hull/crypto/hkdf" | "hull/crypto/otp" | "hull/crypto/sealbox" | "hull/csv"
        | "hull/db" | "hull/email" | "hull/encoding" | "hull/env" | "hull/fs" | "hull/gpu"
        | "hull/http-client" | "hull/http-server" | "hull/i18n" | "hull/image"
        | "hull/inspect" | "hull/jobs" | "hull/json" | "hull/jwt" | "hull/kv" | "hull/log"
        | "hull/logx" | "hull/mime" | "hull/path" | "hull/qrcode" | "hull/retry"
        | "hull/search" | "hull/smtp" | "hull/ssh" | "hull/template" | "hull/time"
        | "hull/timers" | "hull/tui" | "hull/uuid" | "hull/validate"
        | "hull/web/attachment-serve" | "hull/web/auth-flows" | "hull/web/auth-health"
        | "hull/web/cookie" | "hull/web/flash" | "hull/web/form" | "hull/web/htmx"
        | "hull/web/htmx/confirm" | "hull/web/htmx/form" | "hull/web/htmx/inline-edit"
        | "hull/web/htmx/pagination" | "hull/web/htmx/search" | "hull/web/htmx/sort"
        | "hull/web/htmx/table" | "hull/web/htmx/toast"
        | "hull/web/middleware/audit-log" | "hull/web/middleware/auth"
        | "hull/web/middleware/cors" | "hull/web/middleware/csp"
        | "hull/web/middleware/csrf" | "hull/web/middleware/etag"
        | "hull/web/middleware/health" | "hull/web/middleware/idempotency"
        | "hull/web/middleware/inbox" | "hull/web/middleware/logger"
        | "hull/web/middleware/oauth" | "hull/web/middleware/outbox"
        | "hull/web/middleware/ratelimit" | "hull/web/middleware/rbac"
        | "hull/web/middleware/session" | "hull/web/middleware/totp"
        | "hull/web/middleware/transaction" | "hull/web/pagination" | "hull/web/pwned"
        | "hull/web/sse" | "hull/web/ws-client" | "hull/web/ws-server" | "hull/worker";

    /** A module declaration `"<vendor>/<name>@<major>"`; a trailing `?` marks it optional. */
    type ModuleSpec = `${ModuleName}@${number}` | `${ModuleName}@${number}?`;

    /** An outbound network allowlist: hosts (exact, `*`, `*.suffix`, CIDR, `$VAR`) and DSN schemes. */
    interface DynamicAllowlist {
        /** Allowed hosts. */
        hosts?: string[];
        /** Allowed DSN schemes (`"postgres"`, `"sqlite"`, `"valkey"`, ...). */
        schemes?: string[];
    }

    /** A named database connection: a DSN (`"$VAR"` reads a declared variable) or a DSN with options. */
    type ManifestDatabase = string | {
        /** The connection's DSN. */
        dsn: string;
        /** Allow session-altering SQL on this (dedicated) connection. */
        sessionControl?: boolean;
    };

    /** An SSH grant: which hosts (and ports) may be dialled. */
    interface ManifestSshGrant {
        /** Allowed hosts (patterns as for `hosts`). */
        hosts?: string[];
        /** Allowed ports (integers 1..65535). */
        ports?: number[];
    }

    /** The object `app.manifest()` takes. Unknown keys are ignored by the runtime. */
    interface Manifest {
        /** First-party modules the app imports (`"hull/db@1"`); the legacy keyed form is still read. */
        modules?: ModuleSpec[] | Record<string, ModuleSpec>;
        /** Filesystem grants under the app directory (or `@state/`). */
        fs?: {
            /** Paths / globs `fs.read`, `fs.stat`, `fs.list`, `fs.mmap` may reach. */
            read?: string[];
            /** Paths / globs `fs.write` may reach. */
            write?: string[];
        };
        /** Environment variables `env.get` may read (max 32). */
        env?: string[];
        /** Variables readable only through `"$VAR"` references elsewhere in the manifest. */
        secrets?: string[];
        /** Outbound hosts `http.fetch`, `ws.connect`, `smtp.send` may reach. */
        hosts?: string[];
        /** Outbound HTTP defaults. */
        http?: {
            /** Whole-request timeout in ms (default 30 s; clamped to 10 min async / 60 s sync). */
            timeoutMs?: number;
        };
        /** Content-Security-Policy `res.html` adds, or `false` for none. */
        csp?: string | false;
        /** CORS policy. */
        cors?: {
            /** Allowed origins. */
            origins?: string[];
            /** Allowed methods (comma-separated). */
            methods?: string;
            /** Allowed request headers (comma-separated). */
            headers?: string;
            /** Whether credentials are allowed. */
            credentials?: boolean;
            /** Preflight cache lifetime in seconds. */
            maxAge?: number;
        };
        /** Named, internal and dynamic database connections. */
        databases?: {
            /** Name -> DSN; `default` is the app's default database when no `-d` is given. */
            named?: Record<string, ManifestDatabase>;
            /** DSN (or `"$VAR"`) of the database the stdlib keeps its `_hull_*` tables in. */
            internal?: string;
            /** The `db.open(dsn)` allowlist. */
            dynamic?: DynamicAllowlist;
        };
        /** Key-value store grants. */
        kv?: {
            /** The `kv.open({ backend: "valkey", dsn })` allowlist. */
            dynamic?: DynamicAllowlist;
        };
        /** SSH grants for `hull/ssh`. */
        ssh?: {
            /** Destinations `ssh.connect` may dial, and as which users. */
            connect?: ManifestSshGrant & {
                /** Allowed login users. */
                users?: string[];
            };
            /** Relays an SSH tunnel may go through. */
            tunnel?: ManifestSshGrant;
        };
        /** A per-user state directory, reachable as `@state/...`. */
        state?: {
            /** The directory's name: a lower-case letter, then up to 63 of `[a-z0-9._-]`. */
            name: string;
        };
        /** Allow the terminal UI (`hull/tui`). */
        tui?: boolean;
        /** Allow WASM compute. */
        compute?: boolean;
        /** Allow GPU compute (`true`, or a device list / runtime-shader opt-in). */
        gpu?: boolean | {
            /** Device indices the app may use (all when absent). */
            devices?: number[];
            /** Allow runtime WGSL source in a built binary. */
            dynamicShaders?: boolean;
        };
        /** WASM compute ceilings. */
        wasm?: {
            /** Linear-memory heap in bytes. */
            heap?: number;
            /** Stack in bytes. */
            stack?: number;
            /** Instruction gas per call (interpreter only). */
            gas?: number;
            /** Wall-clock limit per call in ms. */
            timeoutMs?: number;
            /** Largest input in bytes. */
            maxInput?: number;
            /** Largest output in bytes. */
            maxOutput?: number;
        };
        /** Run deadlines, input ceilings and request concurrency. */
        limits?: {
            /** Run entries on a dedicated VM thread (serve mode). */
            vmThread?: boolean;
            /** Grace past a run's deadline before the VM thread is abandoned, in ms. */
            vmThreadGraceMs?: number;
            /** Hard ceiling after abandonment before the process exits, in ms. */
            vmThreadCeilingMs?: number;
            /** Deadline of an HTTP handler run, in ms (default 10 s). */
            requestMs?: number;
            /** Deadline of a timer / WebSocket / SSE callback run, in ms (default 5 s). */
            callbackMs?: number;
            /** Deepest JSON nesting a parse accepts. */
            jsonMaxDepth?: number;
            /** Most keys in one JSON object. */
            jsonMaxKeys?: number;
            /** Longest RegExp source, in characters. */
            regexpMaxSource?: number;
            /** Longest `normalize` / `localeCompare` input. */
            normalizeMaxInput?: number;
            /** Requests in flight per client (0 = off). */
            perIpConcurrency?: number;
            /** Open connections per client (0 = off). */
            perIpConnections?: number;
            /** Proxies whose X-Forwarded-For is trusted for the client key. */
            trustedProxies?: string[];
            /** Requests in flight per route pattern (`"/upload"`, `"/api/*"`). */
            routeConcurrency?: Record<string, number>;
        };
        /** Opt in to JIT / runtime code generation (the kernel sandbox then refuses to start). */
        allowDynamicCode?: boolean;
        /** Opt in to `dlopen()` of native libraries (the kernel sandbox then refuses to start). */
        allowDynamicLibraries?: boolean;
    }

    // ── app.main ─────────────────────────────────────────────────────────

    /** `ctx.stdin` of `app.main` (readable only from main's own code; it blocks). */
    interface MainInput {
        /** Read the rest of the input (`"a"`); `""` at end of input. */
        read(mode: "a" | "*a"): string;
        /** Read one line without its newline (`"l"`, the default); `null` at end of input. */
        read(mode?: "l" | "*l"): string | null;
        /** Read up to `count` bytes; `null` at end of input. */
        read(count: number): string | null;
        /** No-op (stdin is never closed). */
        close(): void;
    }

    /** `ctx.stdout` / `ctx.stderr` of `app.main`. */
    interface MainOutput {
        /** Write each argument: a string as UTF-8, bytes as they are. */
        write(...data: (string | number | boolean | Bytes)[]): void;
        /** Flush the stream. */
        flush(): void;
    }

    /** The signal a SIGINT / SIGTERM delivers to `app.main` (`ctx.signal`). */
    interface MainSignal {
        /** Whether a signal has arrived. */
        readonly aborted: boolean;
        /** `"SIGINT"` / `"SIGTERM"` once a signal arrived, else `undefined`. */
        readonly reason: "SIGINT" | "SIGTERM" | undefined;
        /** Called with the reason when a signal arrives; setting it earns main its grace period. */
        onabort: ((reason: "SIGINT" | "SIGTERM") => unknown) | null;
        /** Resolves with the reason when a signal arrives; reading it earns main its grace period. */
        readonly promise: Promise<"SIGINT" | "SIGTERM">;
    }

    /** The context `app.main(fn)` passes to fn. */
    interface MainContext {
        /** Command-line arguments after the app's own (strings). */
        args: string[];
        /** The declared (`manifest.env`) variables that are set. */
        env: Record<string, string>;
        /** Standard input. */
        stdin: MainInput;
        /** Standard output. */
        stdout: MainOutput;
        /** Standard error. */
        stderr: MainOutput;
        /** SIGINT / SIGTERM delivery. */
        signal: MainSignal;
    }

    /** `app.main`'s function: its number is the exit code (0 / nothing continues to serve, if routes exist). */
    type MainFunction = (ctx: MainContext) => number | void | Promise<number | void>;

    // ── Shapes other modules fill ────────────────────────────────────────

    /** A database connection (`db.default()`, `db.connect(name)`, `db.open(dsn)`). */
    interface DbConnection {}
    /** A buffer whose bytes live in WASM linear memory or Hull-owned memory. */
    interface WasmBuffer {}
    /** A read-only memory-mapped file (`fs.mmap`). */
    interface MappedBuffer {}
    /** A decoded image (`hull:image`). */
    interface Image {}
    /** An Error a Hull runtime refusal throws, carrying its refusal code. */
    interface RefusalError extends Error { code: "wait_refused" | "busy" }
}
