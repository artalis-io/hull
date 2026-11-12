// hull:jobs - durable, DB-backed background jobs: queues, retries, cron, workflows, event subscriptions.
declare module "hull:jobs" {
    /** A job's status. */
    export type JobStatus = "pending" | "running" | "done" | "dead" | "blocked" | "waiting";

    /** An outcome sentinel a handler returns (`jobs.RETRY`, `jobs.DEAD`, `jobs.DISCARD`). */
    export interface JobOutcome<K extends "retry" | "dead" | "discard" = "retry" | "dead" | "discard"> {
        /** The outcome it selects. */
        readonly _hullJobsOutcome: K;
    }

    /** A claimed job, as a handler receives it; `D` narrows the payload type (default: `Hull.Data`). */
    export interface Job<D = Hull.Data> {
        /** Job id. */
        id: number;
        /** Job type (the handler dispatch key). */
        type: string;
        /** The decoded payload (null when none). */
        data: D;
        /** Attempts so far, this one included. */
        attempts: number;
        /** The dead-letter threshold. */
        maxAttempts: number;
        /** W3C trace context from `enqueue({ trace })`, or null. */
        trace: string | null;
        /** The queue. */
        queue: string;
        /** Unix seconds of the enqueue. */
        createdAt: number;
        /** The claim's token (what `jobs.heartbeat` checks). */
        claimToken: string;
        /** Results of the jobs it depends on, in `dependsOn` order (null when it has none; set by `jobs.work`). */
        deps?: (Hull.Data | undefined)[] | null;
    }

    /** A job's full status view (`jobs.get`, `jobs.dead`). */
    export interface JobView {
        /** Job id. */
        id: number;
        /** Job type. */
        type: string;
        /** The decoded payload (null when none). */
        data: Hull.Data;
        /** Attempts so far. */
        attempts: number;
        /** The dead-letter threshold. */
        maxAttempts: number;
        /** Trace context, or null. */
        trace: string | null;
        /** The queue. */
        queue: string;
        /** Unix seconds of the enqueue. */
        createdAt: number;
        /** Current status. */
        status: JobStatus;
        /** Priority (higher runs first). */
        priority: number;
        /** Unix seconds it is due. */
        runAt: number;
        /** The last error, or null. */
        lastError: string | null;
        /** Unix seconds of the last change. */
        updatedAt: number;
        /** Progress percent set by `jobs.progress`. */
        progress: number;
    }

    /** A handler: return nothing / data to complete, throw to retry with backoff, or return an outcome sentinel. May be async. */
    export type JobHandler<D = Hull.Data> = (job: Job<D>) => unknown;

    /** Options for `jobs.init`. */
    export interface JobsInitOptions {
        /** Dead-letter threshold (1..10000, default 25). */
        maxAttempts?: number | null;
        /** Seconds before an orphaned running job is reclaimed (default 300). */
        visibilityTimeout?: number;
        /** Minimum seconds between reaper sweeps (default 30; 0 = every work() call). */
        reapInterval?: number;
        /** Retry delay in seconds for a 1-based attempt. */
        backoff?: (attempt: number) => number;
        /** Record per-attempt history: all queues, none, or some. */
        history?: boolean | { queues: readonly string[] };
        /** Seconds to keep attempt rows (null / false = the cleanup default). */
        historyRetention?: number | null | false;
        /** Keep the durable event log (`jobs.events` / `jobs.subscribe`). */
        events?: boolean;
        /** Seconds a subscription drain holds back young events. */
        eventsGrace?: number | null;
    }

    /** Options for `jobs.enqueue` (and `jobs.start`, `enqueueMany` items). */
    export interface EnqueueOptions {
        /** Queue name (default "default"). */
        queue?: string | null;
        /** Priority, an integer (default 0; higher first). */
        priority?: number | null;
        /** Seconds from now before it is due. */
        delay?: number | null;
        /** Unix seconds it is due. */
        runAt?: number | null;
        /** Unix seconds it is due (alias of runAt; wins over it). */
        at?: number | null;
        /** Dead-letter threshold for this job. */
        maxAttempts?: number | null;
        /** Collapse onto an un-run job with the same key in the queue (enqueue then returns null). */
        dedupKey?: string | number | null;
        /** Collapse onto a job with the same dedupKey enqueued in the last `throttle` seconds. */
        throttle?: number | null;
        /** Job ids that must complete first (their results arrive as `job.deps`). */
        dependsOn?: readonly number[] | null;
        /** "run": run even when a dependency failed (default: cascade-fail). */
        onDepFailure?: "run" | "fail";
        /** Key whose running jobs are limited to `concurrency`. */
        concurrencyKey?: string | number | null;
        /** How many jobs with `concurrencyKey` may run at once. */
        concurrency?: number | null;
        /** Enforce `concurrency` fleet-wide with a durable slot. */
        concurrencyStrict?: boolean;
        /** W3C trace context propagated to `job.trace`. */
        trace?: string | null;
    }

    /** One item of `jobs.enqueueMany` (no `dependsOn`). */
    export interface EnqueueManyItem {
        /** Job type. */
        type: string;
        /** Payload. */
        data?: Hull.Data;
        /** Enqueue options. */
        opts?: Omit<EnqueueOptions, "dependsOn" | "onDepFailure">;
    }

    /** Which queues to draw from: one queue, a priority list, or a weight map. */
    export interface QueueSelection {
        /** A single queue (default "default"). */
        queue?: string | null;
        /** A list (strict priority) or a `{ queue: weight }` map (weighted fairness). */
        queues?: readonly string[] | { [queue: string]: number };
    }

    /** Options for `jobs.claim`. */
    export interface ClaimOptions extends QueueSelection {
        /** Most jobs to claim (1..1000, default 10). */
        batch?: number;
    }

    /** Options for `jobs.work`. */
    export interface WorkOptions extends ClaimOptions {
        /** Seconds before an orphaned running job is reclaimed. */
        visibilityTimeout?: number | null;
        /** Minimum seconds between reaper sweeps. */
        reapInterval?: number | null;
    }

    /** Options for `jobs.runWorker`. */
    export interface RunWorkerOptions extends WorkOptions {
        /** Independent claim loops in this process (default 1). */
        concurrency?: number | null;
        /** Sleep between empty polls, in ms (default 1000). */
        pollMs?: number | null;
        /** Return once a poll finds nothing. */
        drain?: boolean;
        /** Return after this many consecutive empty polls. */
        maxEmptyPolls?: number | null;
    }

    /** Job counts by status. */
    export interface JobStats {
        /** Pending jobs. */
        pending: number;
        /** Running jobs. */
        running: number;
        /** Completed jobs. */
        done: number;
        /** Dead-lettered jobs. */
        dead: number;
        /** Jobs waiting on dependencies. */
        blocked: number;
        /** Other statuses present (e.g. "waiting"). */
        [status: string]: number;
    }

    /** Live gauges of one queue (or the totals). */
    export interface QueueGauges {
        /** Pending jobs. */
        pending: number;
        /** Running jobs. */
        running: number;
        /** Workflows parked on a signal. */
        waiting: number;
        /** Jobs waiting on dependencies. */
        blocked: number;
        /** Dead-lettered jobs. */
        dead: number;
        /** Seconds since the oldest ready pending job was enqueued (queues only, when any). */
        oldestPendingAge?: number;
    }

    /** p50 / p95 / p99 of a sample, in ms. */
    export interface Percentiles {
        /** Median. */
        p50: number;
        /** 95th percentile. */
        p95: number;
        /** 99th percentile. */
        p99: number;
    }

    /** What `jobs.metrics` returns. */
    export interface JobMetrics {
        /** Gauges per queue. */
        queues: { [queue: string]: QueueGauges };
        /** Gauges summed over the queues. */
        totals: QueueGauges;
        /** With attempt history: wait / run latency over the window. */
        latency?: { waitMs: Percentiles; runMs: Percentiles };
        /** With attempt history: completions / dead-letters per second over the window. */
        throughput?: { donePerSec: number; deadPerSec: number };
        /** With a non-empty event log: its depth and each subscription's lag. */
        events?: { logDepth: number; subscriptions: { name: string; lag: number; failures: number }[] };
    }

    /** One attempt in `jobs.history`. */
    export interface JobAttempt {
        /** 1-based attempt number. */
        attemptNo: number;
        /** Start, unix ms. */
        startedMs: number;
        /** End, unix ms. */
        finishedMs: number;
        /** Run time, ms. */
        durationMs: number;
        /** Time due before it started, ms. */
        waitMs: number | null;
        /** "done", "retry", "dead", ... */
        outcome: string;
        /** The error, or null. */
        error: string | null;
    }

    /** What `jobs.result` / `jobs.await` return. */
    export interface JobResult {
        /** Current status. */
        status: JobStatus;
        /** A done job's decoded return value (absent when it returned nothing). */
        result?: Hull.Data;
        /** A dead job's last error. */
        error?: string | null;
    }

    /** Options for `jobs.await`. */
    export interface AwaitOptions {
        /** Give up after this many ms and return the non-terminal view. */
        timeout?: number | null;
        /** Poll interval in ms (default 100). */
        interval?: number | null;
    }

    /** Options for `ctx.step`. */
    export interface StepOptions {
        /** Saga compensation, run (newest first) if the workflow dead-letters. */
        compensate?: () => unknown;
    }

    /** The context a workflow body receives. Deterministic helpers are memoized so a replay sees the same values. */
    export interface WorkflowContext<I = Hull.Data> {
        /** The workflow id (a job id). */
        readonly id: number;
        /** The workflow name. */
        readonly name: string;
        /** The input from `jobs.start` (`I` narrows it; default: `Hull.Data`). */
        readonly input: I;
        /** The replay generation. */
        readonly generation: number;
        /** Trace context, or null. */
        readonly trace: string | null;
        /** A memoized unix-seconds clock. */
        now(): Promise<number>;
        /** A memoized Math.random(). */
        random(): Promise<number>;
        /** A memoized random token. */
        uuid(): Promise<string>;
        /** Run `fn` once and memoize its (JSON) result; a replay returns the stored result without calling it. */
        step<T>(name: string, fn: () => T, opts?: StepOptions): Promise<Awaited<T>>;
        /** Durable sleep: parks the workflow (it unwinds and resumes later). Refused inside a transaction. */
        sleep(seconds: number): void;
        /** Wait for `jobs.signal(id, name, payload)`: returns the payload (`T` narrows it), or null on timeout. Parks until then. */
        waitSignal<T = Hull.Data>(name: string, opts?: { timeout?: number }): T | null;
        /** Versioning marker: true for runs that reach it past the recorded prefix. */
        patched(patchId: string): boolean;
    }

    /** A workflow body; its return value becomes the workflow's result. */
    export type WorkflowBody<I = Hull.Data> = (ctx: WorkflowContext<I>) => unknown;

    /** What `jobs.workflowStatus` returns. */
    export interface WorkflowStatus {
        /** Current status. */
        status: JobStatus;
        /** The workflow name. */
        name: string;
        /** Completed step names. */
        stepsDone: string[];
        /** Unix seconds of the start. */
        startedAt: number;
        /** Unix seconds of the last change. */
        updatedAt: number;
        /** "signal", or "sleep:<unix seconds>" while parked. */
        waitingFor?: string;
        /** A done workflow's result. */
        result?: Hull.Data;
        /** A dead workflow's error. */
        error?: string | null;
    }

    /** Options for `jobs.limit` (null / false removes the limit). */
    export interface LimitOptions {
        /** Jobs per window (a whole number >= 1). */
        rate: number;
        /** Window in seconds (default 1). */
        per?: number;
    }

    /** Options for `jobs.cron`. */
    export interface CronOptions {
        /** Job type enqueued (default: the schedule name). */
        type?: string | null;
        /** Queue. */
        queue?: string | null;
        /** Priority. */
        priority?: number | null;
        /** Dead-letter threshold. */
        maxAttempts?: number | null;
        /** Fixed UTC offset: minutes, or "Z" / "UTC" / "+05:30" style. */
        tz?: number | string | null;
    }

    /** Options for `jobs.dead`. */
    export interface DeadOptions {
        /** Only this queue. */
        queue?: string | null;
        /** Rows (1..1000, default 100). */
        limit?: number;
        /** Rows to skip. */
        offset?: number;
    }

    /** Options for `jobs.cleanup`. */
    export interface CleanupOptions {
        /** Only this queue. */
        queue?: string | null;
        /** Retention in seconds (default 604800). */
        olderThan?: number | null;
        /** Absolute cutoff, unix seconds (wins over olderThan). */
        before?: number | null;
        /** Statuses to purge (default ["done", "dead"]). */
        statuses?: readonly JobStatus[];
    }

    /** A durable event type. */
    export type JobEventType = "enqueued" | "completed" | "retried" | "dead";

    /** A row of the durable event log. */
    export interface JobEvent {
        /** Event id (increasing). */
        id: number;
        /** Unix seconds. */
        ts: number;
        /** Event type. */
        type: string;
        /** The job's id. */
        job_id: number;
        /** The job's type. */
        job_type: string;
        /** The job's queue. */
        queue: string;
        /** `{ error?, attempt?, trace? }` decoded, the raw text if it does not decode, or null. */
        data: Hull.Json;
    }

    /** Options for `jobs.events`. */
    export interface EventsOptions {
        /** Only events after this id. */
        since?: number;
        /** Only these types. */
        types?: readonly string[] | null;
        /** Rows (1..1000, default 100). */
        limit?: number;
    }

    /** Options for `jobs.subscribe`. */
    export interface SubscribeOptions {
        /** Only these event types. */
        types?: readonly string[] | null;
        /** Where a new subscription starts (default "now"). */
        from?: "now" | "beginning";
        /** Skip an event after this many failed deliveries. */
        maxFailures?: number;
    }

    /** `jobs.on` listener info. */
    export interface CompletedInfo {
        /** The handler's return value. */
        result: Hull.Data;
    }
    /** `jobs.on("retried")` listener info. */
    export interface RetriedInfo {
        /** The error text. */
        error: string;
        /** The attempt that failed. */
        attempt: number;
    }
    /** `jobs.on("dead")` listener info. */
    export interface DeadInfo {
        /** The error text. */
        error: string;
    }

    /** The `hull:jobs` module object. Registration and setter calls return it for chaining. */
    export interface JobsModule {
        /** Create the tables (idempotent) and set the options. */
        init(opts?: JobsInitOptions): JobsModule;
        /** Enqueue a job (joins the caller's db.batch); the id, or null when deduplicated. */
        enqueue(type: string, data?: Hull.Data, opts?: EnqueueOptions | null): number | null;
        /** Enqueue many jobs in one transaction; ids in order (null per deduplicated item). */
        enqueueMany(items: readonly EnqueueManyItem[]): (number | null)[];
        /** Atomically claim ready jobs, marking them running (low level; `work` wraps it). */
        claim(opts?: ClaimOptions): Job[];
        /** Register the handler for a job type. */
        handler<D = Hull.Data>(type: string, fn: JobHandler<D>): JobsModule;
        /** Register the catch-all handler (unhandled types otherwise dead-letter). */
        default<D = Hull.Data>(fn: JobHandler<D>): JobsModule;
        /** Register an in-process listener for completed jobs. */
        on(event: "completed", fn: (job: Job, info: CompletedInfo) => void): JobsModule;
        /** Register an in-process listener for failed attempts that will be retried. */
        on(event: "retried", fn: (job: Job, info: RetriedInfo) => void): JobsModule;
        /** Register an in-process listener for dead-lettered jobs. */
        on(event: "dead", fn: (job: Job, info: DeadInfo) => void): JobsModule;
        /** Reap, claim a batch and run each handler; resolves to the number of jobs processed. */
        work(opts?: WorkOptions): Promise<number>;
        /** Reclaim jobs stuck running past the visibility timeout; returns how many. */
        reap(opts?: { visibilityTimeout?: number | null }): number;
        /** Job counts by status. */
        stats(opts?: { queue?: string | null }): JobStats;
        /** The dedicated-worker loop (from app.main); resolves to the jobs processed once stopped / drained. */
        runWorker(opts?: RunWorkerOptions): Promise<number>;
        /** Ask the running worker loop to stop after this iteration. */
        stop(): void;
        /** A job's status view, or null. */
        get(id: number): JobView | null;
        /** A job's terminal result view, or null when unknown. */
        result(id: number): JobResult | null;
        /** Wait (polling) until the job is done / dead, or the timeout passes; null when unknown. */
        await(id: number, opts?: AwaitOptions): Promise<JobResult | null>;
        /** Set a job's progress percent; returns the clamped value. */
        progress(id: number, pct: number): number;
        /** Extend a running job's claim; false once the claim is lost. */
        heartbeat(job: Job<unknown>): boolean;
        /** Set (or with null / false remove) a fleet-wide rate limit for a queue. */
        limit(queue: string, opts: LimitOptions | null | false): JobsModule;
        /** DB-derived gauges, latency and throughput (window in seconds, default 300). */
        metrics(opts?: { queue?: string | null; window?: number | null }): JobMetrics;
        /** A job's attempt timeline (empty without history). */
        history(id: number): JobAttempt[];
        /** Pause a queue fleet-wide. */
        pause(queue: string): JobsModule;
        /** Resume a paused queue. */
        resume(queue: string): JobsModule;
        /** Delete a queue's jobs (pending by default); returns how many. */
        purge(queue: string, opts?: { statuses?: readonly JobStatus[] | null }): number;
        /** Register a durable workflow. */
        workflow<I = Hull.Data>(name: string, fn: WorkflowBody<I>): JobsModule;
        /** Start a workflow instance; its id, or null when deduplicated. */
        start(name: string, input?: Hull.Data, opts?: EnqueueOptions | null): number | null;
        /** Deliver a signal to a workflow; whether it was stored. */
        signal(id: number, name: string, payload?: Hull.Data, opts?: { deliveryId?: string } | null): boolean;
        /** A workflow's state, or null. */
        workflowStatus(id: number): WorkflowStatus | null;
        /** Dead-lettered jobs, newest first. */
        dead(opts?: DeadOptions): JobView[];
        /** Requeue a dead job; whether it was. */
        retry(id: number): boolean;
        /** Delete a still-pending job; whether it was. */
        cancel(id: number): boolean;
        /** Purge old terminal jobs (and their history / events); returns jobs deleted. */
        cleanup(opts?: CleanupOptions): number;
        /** Register or update a durable 5-field cron schedule (UTC unless `tz`). */
        cron(name: string, spec: string, data?: Hull.Data, opts?: CronOptions | null): JobsModule;
        /** Remove a cron schedule; whether one existed. */
        uncron(name: string): boolean;
        /** Tail of the durable event log, newest first. */
        events(opts?: EventsOptions): JobEvent[];
        /** Register a durable named subscription (handler synchronous, at-least-once). */
        subscribe(name: string, handler: (event: JobEvent) => void, opts?: SubscribeOptions): JobsModule;
        /** Remove a subscription and its cursor. */
        unsubscribe(name: string): JobsModule;
        /** Outcome: retry with backoff. */
        readonly RETRY: JobOutcome<"retry">;
        /** Outcome: dead-letter now. */
        readonly DEAD: JobOutcome<"dead">;
        /** Outcome: drop the job (done, no result). */
        readonly DISCARD: JobOutcome<"discard">;
    }

    /** Durable jobs. */
    export const jobs: JobsModule;
    /** Durable jobs (default export). */
    const _default: JobsModule;
    export default _default;

    /** Outcome: retry with backoff. */
    export const RETRY: JobOutcome<"retry">;
    /** Outcome: dead-letter now. */
    export const DEAD: JobOutcome<"dead">;
    /** Outcome: drop the job. */
    export const DISCARD: JobOutcome<"discard">;
    /** See `JobsModule.init`. */
    export const init: JobsModule["init"];
    /** See `JobsModule.enqueue`. */
    export const enqueue: JobsModule["enqueue"];
    /** See `JobsModule.enqueueMany`. */
    export const enqueueMany: JobsModule["enqueueMany"];
    /** See `JobsModule.claim`. */
    export const claim: JobsModule["claim"];
    /** See `JobsModule.handler`. */
    export const handler: JobsModule["handler"];
    /** See `JobsModule.on`. */
    export const on: JobsModule["on"];
    /** See `JobsModule.work`. */
    export const work: JobsModule["work"];
    /** See `JobsModule.reap`. */
    export const reap: JobsModule["reap"];
    /** See `JobsModule.stats`. */
    export const stats: JobsModule["stats"];
    /** See `JobsModule.runWorker`. */
    export const runWorker: JobsModule["runWorker"];
    /** See `JobsModule.stop`. */
    export const stop: JobsModule["stop"];
    /** See `JobsModule.get`. */
    export const get: JobsModule["get"];
    /** See `JobsModule.result`. */
    export const result: JobsModule["result"];
    /** See `JobsModule.progress`. */
    export const progress: JobsModule["progress"];
    /** See `JobsModule.heartbeat`. */
    export const heartbeat: JobsModule["heartbeat"];
    /** See `JobsModule.limit`. */
    export const limit: JobsModule["limit"];
    /** See `JobsModule.metrics`. */
    export const metrics: JobsModule["metrics"];
    /** See `JobsModule.history`. */
    export const history: JobsModule["history"];
    /** See `JobsModule.pause`. */
    export const pause: JobsModule["pause"];
    /** See `JobsModule.resume`. */
    export const resume: JobsModule["resume"];
    /** See `JobsModule.purge`. */
    export const purge: JobsModule["purge"];
    /** See `JobsModule.workflow`. */
    export const workflow: JobsModule["workflow"];
    /** See `JobsModule.start`. */
    export const start: JobsModule["start"];
    /** See `JobsModule.signal`. */
    export const signal: JobsModule["signal"];
    /** See `JobsModule.workflowStatus`. */
    export const workflowStatus: JobsModule["workflowStatus"];
    /** See `JobsModule.dead`. */
    export const dead: JobsModule["dead"];
    /** See `JobsModule.retry`. */
    export const retry: JobsModule["retry"];
    /** See `JobsModule.cancel`. */
    export const cancel: JobsModule["cancel"];
    /** See `JobsModule.cleanup`. */
    export const cleanup: JobsModule["cleanup"];
    /** See `JobsModule.cron`. */
    export const cron: JobsModule["cron"];
    /** See `JobsModule.uncron`. */
    export const uncron: JobsModule["uncron"];
    /** See `JobsModule.events`. */
    export const events: JobsModule["events"];
    /** See `JobsModule.subscribe`. */
    export const subscribe: JobsModule["subscribe"];
    /** See `JobsModule.unsubscribe`. */
    export const unsubscribe: JobsModule["unsubscribe"];
}
