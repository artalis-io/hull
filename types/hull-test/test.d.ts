// The `test` global of `hull test` files. Reference this file from test files only.

declare namespace Hull {
    /** Options of an in-process test request (`test.get(path, opts)`). */
    interface TestRequestOptions {
        /** Request headers. */
        headers?: Record<string, string>;
        /** Request body (a string). */
        body?: string;
        /** Initial `req.ctx` (JSON-encoded on the way in). */
        ctx?: Record<string, unknown>;
        /** Run the app's middleware too (default false: the handler only). */
        middleware?: boolean;
    }

    /** The response of an in-process test request; `T` (unchecked, default `Hull.Data`) types its JSON body. */
    interface TestResponse<T = Data> {
        /** Status code. */
        status: number;
        /** Response body; absent when empty. */
        body?: string;
        /** The body parsed as JSON, when it starts with `{` or `[` and parses. */
        json?: T;
        /** Response headers by lower-case name; absent when there are none. */
        headers?: Record<string, string>;
    }

    /** Options of `test.runMain`. */
    interface TestRunMainOptions {
        /** What `ctx.stdin` reads. */
        stdin?: string | Bytes;
        /** Exactly `ctx.env`; every name must be in the manifest's `env`. */
        env?: Record<string, string>;
        /** Bound on main's waits in ms (default 10 s, never past the case's deadline). */
        timeoutMs?: number;
    }

    /** What a `test.execStub` answers; omitted fields take their defaults. */
    interface TestExecStubResult {
        status?: number;
        signal?: string | null;
        /** A byte string (code units 0..255) or bytes. */
        stdout?: string | ArrayBuffer;
        stderr?: string | ArrayBuffer;
        timedOut?: boolean;
        /** A stream command: it ended at its `idleMs` (sets `timedOut` too). */
        idleTimedOut?: boolean;
        durationMs?: number;
        /**
         * A stream command (`exec.stream`): its output, delivered in order,
         * one per read, then the result (cut and killed at the command's
         * totals, as the broker does). `stdout` / `stderr` are not used.
         */
        chunks?: TestExecStubChunk[];
        /**
         * A stream command opened with `stdinOpen`: called with the bytes of
         * each write; the chunks it returns are read next.
         */
        onWrite?: (bytes: Uint8Array) => TestExecStubChunk[] | undefined | null | void;
    }

    /** One chunk of a stream stub's output. */
    interface TestExecStubChunk {
        fd: "stdout" | "stderr";
        /** A string (UTF-8) or bytes. */
        data: string | ArrayBuffer | ArrayBufferView;
    }

    /** The result of `test.runMain`. */
    interface TestRunMainResult {
        /** main's exit code (1 when it threw or rejected). */
        code: number;
        /** Captured stdout, decoded as UTF-8. */
        stdout: string;
        /** Captured stderr, decoded as UTF-8. */
        stderr: string;
        /** Captured stdout bytes as written. */
        stdoutBytes: ArrayBuffer;
        /** Captured stderr bytes as written. */
        stderrBytes: ArrayBuffer;
    }

    /** A build plugin action for `test.runTransform`, answered by a stub host. */
    interface TestTransformSpec {
        /** `ctx.config`. */
        config?: Record<string, unknown>;
        /** `ctx.meta`. */
        meta?: Record<string, unknown>;
        /** What `ctx.source.units()` returns. */
        units?: unknown[];
        /** The workspace `ctx.inputs` reads: path -> contents. */
        inputs?: Record<string, string | Bytes>;
        /** What `ctx.source.erasedSource(unit)` answers, by unit id: the erased bytes, or
         *  `{ refusals }`. */
        erased?: Record<string, string | Bytes | { refusals: Record<string, unknown>[] }>;
    }

    /** One output the transform declared. */
    interface TestTransformOutput {
        declared: "generated" | "artifact";
        descriptor: Record<string, unknown>;
        /** The payload's name (`createBlob` / `createTree`). */
        name: string;
        /** A blob's bytes. */
        bytes?: ArrayBuffer;
        /** A blob's text, when every write was a string. */
        text?: string;
        /** A tree's entries. */
        entries?: { path: string; mode: string; bytes: ArrayBuffer; text?: string }[];
    }

    /** The result of `test.runTransform`. */
    interface TestTransformResult {
        outputs: TestTransformOutput[];
        diagnostics: Record<string, unknown>[];
        /** The transform's error message, when it threw. */
        error?: string;
    }

    /** Options of a test case. */
    interface TestCaseOptions {
        /** Per-test timeout of an async body, in ms (default 5000). */
        timeout?: number;
    }

    /** A test case body: throw (or reject) to fail. */
    type TestBody = () => unknown;

    /** The `test` global. */
    interface TestGlobal {
        /** Register a test case. */
        (name: string, fn: TestBody): void;
        /** Register a test case with options. */
        (name: string, opts: TestCaseOptions, fn: TestBody): void;
        /** In-process GET through the app's routes. */
        get<T = Data>(path: string, opts?: TestRequestOptions): TestResponse<T>;
        /** In-process POST through the app's routes. */
        post<T = Data>(path: string, opts?: TestRequestOptions): TestResponse<T>;
        /** In-process PUT through the app's routes. */
        put<T = Data>(path: string, opts?: TestRequestOptions): TestResponse<T>;
        /** In-process DELETE through the app's routes. */
        delete<T = Data>(path: string, opts?: TestRequestOptions): TestResponse<T>;
        /** In-process PATCH through the app's routes. */
        patch<T = Data>(path: string, opts?: TestRequestOptions): TestResponse<T>;
        /** Assert `actual === expected` (objects by identity, never deeply); throws on mismatch. */
        eq(actual: unknown, expected: unknown, message?: string): void;
        /** Assert truthiness; throws otherwise. */
        ok(value: unknown, message?: string): void;
        /** Assert that `fn` throws (and its message contains `pattern`, when given). */
        err(fn: () => unknown, pattern?: string): void;
        /** Run `app.main` once with captured stdio. */
        runMain(args?: string[], opts?: TestRunMainOptions): TestRunMainResult;
        /**
         * Answer `exec.run(name, ...)` / `exec.stream(name, ...)` in this
         * file's tests: a result object, or a function `(args, name, stdin) =>
         * result` (`stdin` the call's stdin as an ArrayBuffer, or undefined);
         * null removes the stub. The call is still matched against the
         * declared command first. For `exec.stream` the result carries
         * `chunks` (and `onWrite`); nothing waits.
         */
        execStub(name: string,
                 result: TestExecStubResult
                       | ((args: string[], name: string, stdin?: ArrayBuffer) => TestExecStubResult)
                       | null): void;
        /** Run a build plugin's transform once against a stub host. */
        runTransform(spec?: TestTransformSpec): Promise<TestTransformResult>;
    }
}

/** The test harness (`hull test` files only). */
declare var test: Hull.TestGlobal;
