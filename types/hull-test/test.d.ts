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
        /** Assert equality (primitives by value, objects by identity); throws on mismatch. */
        eq(actual: unknown, expected: unknown, message?: string): void;
        /** Assert truthiness; throws otherwise. */
        ok(value: unknown, message?: string): void;
        /** Assert that `fn` throws (and its message contains `pattern`, when given). */
        err(fn: () => unknown, pattern?: string): void;
        /** Run `app.main` once with captured stdio. */
        runMain(args?: string[], opts?: TestRunMainOptions): TestRunMainResult;
    }
}

/** The test harness (`hull test` files only). */
declare var test: Hull.TestGlobal;
