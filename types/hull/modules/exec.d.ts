// hull:exec - run a command the manifest's `exec` section declares
// (docs/design/exec_broker.md), through the command broker (native Linux
// and macOS).
// Under `hull test` a call is answered only by a stub (test.execStub).
declare module "hull:exec" {
    /** What one run of a declared command produced. */
    interface ExecResult {
        /** The exit status (0 when it was killed by a signal). */
        readonly status: number;
        /** The signal that ended it ("SIGKILL"), or null. */
        readonly signal: string | null;
        /** stdout as a byte string: one code unit per byte. */
        readonly stdout: string;
        /** stderr as a byte string. */
        readonly stderr: string;
        /** stdout as bytes. */
        readonly stdoutBytes: ArrayBuffer;
        /** It ran past the command's `timeoutMs` and was killed. */
        readonly timedOut: boolean;
        /** stdout was cut to the command's `stdoutMax`. */
        readonly stdoutTruncated: boolean;
        /** stderr was cut to the command's `stderrMax`. */
        readonly stderrTruncated: boolean;
        /** How long it ran, in ms. */
        readonly durationMs: number;
    }

    /** Options for one run. */
    interface ExecRunOptions {
        /**
         * Written to the command's stdin (a string is sent as UTF-8). Needs
         * the command's `stdinMax`; never logged. Without it the command
         * reads /dev/null.
         */
        stdin?: string | ArrayBuffer | ArrayBufferView;
    }

    /** One piece of a stream's output, as the command wrote it. */
    interface ExecChunk {
        readonly fd: "stdout" | "stderr";
        /** At most 64 KiB. */
        readonly data: Uint8Array;
    }

    /** How a streamed call ended. */
    interface ExecStreamResult {
        /** The exit status (0 when it was killed by a signal). */
        readonly status: number;
        /** The signal that ended it ("SIGKILL" after close()), or null. */
        readonly signal: string | null;
        /** It ran past the command's `timeoutMs` and was killed. */
        readonly timedOut: boolean;
        /** stdout reached the command's `stdoutMax` total and it was killed. */
        readonly stdoutTruncated: boolean;
        /** stderr reached the command's `stderrMax` total and it was killed. */
        readonly stderrTruncated: boolean;
        /** Bytes it wrote to stdout (within the cap). */
        readonly stdoutLength: number;
        /** Bytes it wrote to stderr (within the cap). */
        readonly stderrLength: number;
        /** How long it ran, in ms. */
        readonly durationMs: number;
    }

    /** Options for one streamed call. */
    interface ExecStreamOptions {
        /** Written to its stdin first (up to 1 MiB); needs the command's `stdinMax`. */
        stdin?: string | ArrayBuffer | ArrayBufferView;
        /** Keep stdin open for write() until closeWrite() (needs `stdinMax`). */
        stdinOpen?: boolean;
    }

    /**
     * A running stream command. An app that stops reading stops the command
     * (back pressure); a stream that is closed, collected, or whose request
     * goes away kills it.
     */
    interface ExecStream extends AsyncIterable<ExecChunk> {
        /** The next chunk, or null once both pipes are closed. */
        read(): Promise<ExecChunk | null>;
        /**
         * Write to its stdin (at most 1 MiB a call); resolves once the broker
         * has the bytes. Needs `stdinOpen`.
         */
        write(bytes: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        /** EOF on its stdin, once queued bytes are written. */
        closeWrite(): void;
        /** Kill the command; later reads give null. */
        close(): void;
        /** How it ended, once it has. */
        result(): Promise<ExecStreamResult>;
        next(): Promise<IteratorResult<ExecChunk, undefined>>;
        return(): Promise<IteratorResult<ExecChunk, undefined>>;
        [Symbol.asyncIterator](): ExecStream;
    }

    export const exec: {
        /**
         * Run the declared command `name`. `args` fill its slots in order;
         * a name that is not declared, an argument a slot does not admit, or
         * a `stdin` longer than the command's `stdinMax` throws a TypeError
         * before anything runs.
         */
        readonly run: (name: string, args?: readonly string[] | null,
                       opts?: ExecRunOptions) => Promise<ExecResult>;
        /**
         * Start the declared `stream` command `name` and return its stream at
         * once. A command that does not declare `stream: true` throws a
         * TypeError (exec.run runs it).
         */
        readonly stream: (name: string, args?: readonly string[] | null,
                          opts?: ExecStreamOptions) => ExecStream;
    };
}
