// hull:exec - run a command the manifest's `exec` section declares
// (docs/design/exec_broker.md), through the command broker (native Linux).
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

    export const exec: {
        /**
         * Run the declared command `name`. `args` fill its slots in order;
         * a name that is not declared, or an argument a slot does not admit,
         * throws a TypeError before anything runs.
         */
        readonly run: (name: string, args?: readonly string[]) => Promise<ExecResult>;
    };
}
