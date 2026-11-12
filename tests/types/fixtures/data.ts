// Type fixture: hull:db, hull:kv, hull:search, hull:cache, hull:blob, hull:attachment,
// hull:jobs, hull:config, hull:retry, hull:logx - used the way an app uses them.
import { db } from "hull:db";
import type { DbParam } from "hull:db";
import { kv } from "hull:kv";
import kvDefault from "hull:kv";
import { search } from "hull:search";
import { cache } from "hull:cache";
import { blob } from "hull:blob";
import { attachment } from "hull:attachment";
import type { AttachmentPart } from "hull:attachment";
import { jobs, enqueue, RETRY } from "hull:jobs";
import type { Job, WorkflowContext } from "hull:jobs";
import { config } from "hull:config";
import { retry } from "hull:retry";
import { logx } from "hull:logx";

interface UserRow { id: number; email: string; avatar: ArrayBuffer | null }

export function useDb(): void {
    const conn = db.default();
    const n: number = conn.exec("INSERT INTO users (email, avatar) VALUES (?, ?)",
                                ["a@b.c", new Uint8Array([1, 2])]);
    const id: number = conn.lastId();
    const rows = conn.query("SELECT * FROM users WHERE id = ?", [id]);
    const v = rows[0].email;            // DbValue
    if (typeof v === "string") v.toUpperCase();
    const users = conn.query<UserRow>("SELECT id, email, avatar FROM users");
    const email: string = users[0].email;
    conn.batch(() => {
        conn.exec("UPDATE users SET email = ? WHERE id = ?", [email, 9007199254740993n]);
    });
    // @ts-expect-error -- an async fn would commit its statements after the first await
    conn.batch(async () => { conn.exec("DELETE FROM users"); });
    const inserted: number = conn.insertIfAbsent("users", ["email"], ["email"], ["x@y.z"]);
    conn.upsert("users", ["id"], ["id", "email"], [1, null]);
    const cols: string[] = conn.tableColumns("users");
    const q: string = conn.quoteIdentifier("order");
    const inTx: boolean = conn.inTransaction();
    if (conn.dialect.supportsNotify) conn.waitNotify("jobs", 1000).then((hit: boolean) => hit);
    const be: "sqlite" | "postgres" | "mysql" | "duckdb" = conn.backendName;
    const ddl: string = conn.autoincrementIdDdl;
    if (conn.udf) {
        conn.udf.register("hull_double", (x) => (typeof x === "number" ? x * 2 : null),
                          { deterministic: true });
        conn.udf.register("hull_total", {
            step(state, x) { state.sum = (Number(state.sum) || 0) + Number(x); },
            finalize(state) { return Number(state.sum) || 0; },
        });
        conn.udf.unregister("hull_double");
        // @ts-expect-error -- UDF names must start with hull_
        conn.udf.register("double", (x) => x);
    }
    // @ts-expect-error -- udf is absent on non-SQLite backends: probe it first
    conn.udf.register("hull_x", () => 1);
    // @ts-expect-error -- an object is not a bindable parameter
    conn.query("SELECT ?", [{ a: 1 }]);
    // @ts-expect-error -- sql is required
    conn.exec();
    // @ts-expect-error -- backendName is a property, not a method
    conn.backendName();
    const p: DbParam[] = [1, "a", true, null, undefined, 2n, new ArrayBuffer(1)];
    void [n, rows, inserted, cols, q, inTx, be, ddl, p];
}

export async function useDbAsync(): Promise<void> {
    const conn = db.connect("analytics");
    const rows = await conn.async.query<{ total: number }>("SELECT COUNT(*) AS total FROM events");
    const total: number = rows[0].total;
    const r = await conn.async.exec("DELETE FROM events WHERE ts < ?", [0]);
    const changed: number = r.changes + r.lastId;
    // @ts-expect-error -- a Promise is not the rows
    const bad: { total: number }[] = conn.async.query("SELECT 1");
    const owned = db.open("sqlite://tenant.db");
    owned.query("SELECT 1");
    owned.close();
    // @ts-expect-error -- only db.open() connections are closed by the app
    conn.close();
    void [total, changed, bad];
}

export function useKvCache(): void {
    const store = kv.open({ backend: "sqlite", database: db.default(), namespace: "agent", defaultTtl: 3600 });
    store.set("run:1", "payload", { ttl: 60 });
    const got: string | null = store.get("run:1");
    const hits: number = store.incr("hits", 1);
    const swapped: boolean = store.cas("run:1", got, "next");
    const keys: string[] = store.scan("run:", { limit: 10 });
    if (store.caps.scan) store.cleanup();
    const st = store.stats();
    const items: number | undefined = st.items;
    store.close();
    const enc = kvDefault.open({ namespace: "secrets", encrypt: { keys: { 1: "k".repeat(32) }, current: 1 } });
    const resealed: number = enc.rekey();
    // @ts-expect-error -- unknown backend
    kv.open({ backend: "mongo" });
    // @ts-expect-error -- options are required
    kv.open();
    // @ts-expect-error -- values are byte strings
    store.set("k", 42);

    const u = cache.fetch("user:1", 300, () => ({ id: 1 }));
    const uid: number = u.id;
    cache.set("k", [1, 2], 60);
    const cached = cache.get<number[]>("k");
    const local = cache.new({ maxEntries: 100, defaultTtl: 30 });
    local.fetch("x", () => "y");
    const size: number = local.size();
    const bc = cache.open({ backend: "memory", namespace: "pages", maxItems: 500 });
    const page: string = bc.fetch("/", 60, () => "<html>");
    // @ts-expect-error -- a byte cache stores strings
    bc.fetch("/x", () => 1);
    void [hits, swapped, keys, items, resealed, uid, cached, size, page];
}

export function useSearch(): void {
    search.createIndex("posts", ["title", "body"], { tokenize: "porter unicode61" });
    search.index("posts", 1, { title: "Hello", body: "world", draft: null });
    const hits = search.query("posts", "hello", { limit: 10, snippet: { tokens: 8 }, order: "rank" });
    if (hits.error) return;
    for (const h of hits) { const r: number = h.rank; const s: string | undefined = h.snippet; void [r, s]; }
    search.reindex("posts", "articles", { columns: { title: "title", body: "content" } });
    search.remove("posts", 1);
    search.dropIndex("posts");
    // @ts-expect-error -- order is rank or rowid
    search.query("posts", "x", { order: "date" });
    // @ts-expect-error -- field values are strings or numbers
    search.index("posts", 2, { title: { text: "x" } });
}

export async function useBlobAttachment(part: AttachmentPart): Promise<void> {
    blob.init({ dir: "data/blobs" });
    const { id, size } = blob.put("hello", { durable: true });
    const w = blob.writer({ expected: id });
    w.write(new Uint8Array([1])).write("more");
    const fin = w.finalize();
    const bytes: ArrayBuffer | null = blob.get(fin.id, { trackAccess: false });
    const r = blob.reader(id);
    for (let c = r.read(4096); c !== null; c = r.read(4096)) void c.byteLength;
    r.close();
    const all = blob.iter();
    const res = blob.cleanup({ maxTotalSize: 1 << 30, strategy: "lru", dryRun: true });
    const freed: number = res.freedBytes + blob.totalSize() + blob.count();
    // @ts-expect-error -- strategy is lru or fifo
    blob.cleanup({ strategy: "random" });
    // @ts-expect-error -- dir is required
    blob.init({});

    attachment.init({ maxSize: 10 << 20, mimeAllowlist: ["image/png"] });
    const aid: string = await attachment.store(part, { uploadedBy: "user-1" });
    const meta = attachment.metadata(aid);
    if (meta) { const m: string = meta.mime; const sz: number = meta.size; void [m, sz]; }
    const data: ArrayBuffer | null = attachment.read(aid);
    const written: number | null = attachment.readToFile(aid, "exports/a.bin");
    const swept: number = attachment.sweep({ limit: 10 });
    const gone: boolean = attachment.delete(aid);
    // @ts-expect-error -- store resolves asynchronously
    const sync: string = attachment.store(part);
    void [size, bytes, all, freed, data, written, swept, gone, sync];
}

export async function useJobs(): Promise<void> {
    jobs.init({ maxAttempts: 5, history: true, events: true, backoff: (n) => n * 10 });
    jobs.handler("send_email", (job: Job) => {
        const to = job.data;
        if (job.attempts > 3) return jobs.DEAD;
        if (to === null) return RETRY;
        return { sent: true };
    }).default(() => jobs.DISCARD);
    jobs.handler<{ to: string }>("welcome", (job) => job.data.to.toUpperCase());
    jobs.workflow<{ amount: number }>("refund", async (ctx) => {
        const amount: number = ctx.input.amount;
        const sig = ctx.waitSignal<{ ok: boolean }>("approved");
        return sig ? amount : 0;
    });
    jobs.handler("untyped", (job) => job.data.to); // the default payload type is Hull.Data
    // @ts-expect-error -- a typed payload is checked
    jobs.handler<{ to: string }>("typed", (job) => job.data.from);
    jobs.on("retried", (job, info) => { const a: number = info.attempt; void [job.id, a]; });
    const id = jobs.enqueue("send_email", { to: "a@b.c" }, { queue: "mail", delay: 30, dedupKey: "welcome:1" });
    const ids = jobs.enqueueMany([{ type: "send_email", data: { to: "x" } }, { type: "ping" }]);
    const viaNamed = enqueue("ping");
    if (id !== null) {
        const view = jobs.get(id);
        if (view) { const s: string = view.status; void s; }
        const res = await jobs.await(id, { timeout: 5000 });
        if (res && res.status === "done") void res.result;
        jobs.progress(id, 50);
        void jobs.history(id)[0]?.durationMs;
    }
    const done: number = await jobs.work({ batch: 20, queues: { mail: 3, default: 1 } });
    await jobs.runWorker({ concurrency: 4, drain: true });
    jobs.stop();
    const m = jobs.metrics();
    const backlog = m.queues.mail?.oldestPendingAge;
    const st = jobs.stats({ queue: "mail" });
    const pending: number = st.pending;
    jobs.limit("mail", { rate: 10, per: 1 }).pause("mail").resume("mail");
    jobs.cron("nightly", "0 3 * * *", { full: true }, { tz: "+02:00" });
    const removed: boolean = jobs.uncron("nightly");
    jobs.subscribe("audit", (ev) => { const t: string = ev.type; void t; }, { types: ["dead"], from: "beginning" });
    for (const ev of jobs.events({ limit: 10 })) void ev.job_id;
    jobs.unsubscribe("audit");
    const purged: number = jobs.purge("mail", { statuses: ["pending", "dead"] }) + jobs.cleanup({ olderThan: 86400 });
    for (const d of jobs.dead({ limit: 5 })) jobs.retry(d.id);

    jobs.workflow("order", async (ctx: WorkflowContext) => {
        const charge = await ctx.step("charge", async () => ({ ok: true, at: await ctx.now() }),
                                      { compensate: () => undefined });
        const ok: boolean = charge.ok;
        const shipped = ctx.waitSignal("shipped", { timeout: 86400 });
        if (ctx.patched("v2")) ctx.sleep(60);
        return { ok, shipped, ref: await ctx.uuid() };
    });
    const wid = jobs.start("order", { amount: 4200 });
    if (wid !== null) {
        jobs.signal(wid, "shipped", { tracking: "1Z" }, { deliveryId: "evt-1" });
        const ws = jobs.workflowStatus(wid);
        if (ws) void ws.stepsDone.length;
    }
    // @ts-expect-error -- work resolves asynchronously
    const syncDone: number = jobs.work();
    // @ts-expect-error -- unknown lifecycle event
    jobs.on("started", () => undefined);
    // @ts-expect-error -- the job type is a string
    jobs.enqueue(42, { to: "a@b.c" });
    // @ts-expect-error -- misspelled option
    jobs.enqueue("t", null, { dedupeKey: "x" });
    void [ids, viaNamed, done, backlog, pending, removed, purged, syncDone];
}

export async function useConfigRetryLogx(): Promise<void> {
    const port: number = config.get("PORT", { type: "integer", default: 8080 });
    const host: string | undefined = config.get("HOST");
    const debug: boolean = config.get("DEBUG", { type: "boolean", required: true });
    const dsn: string = config.require("DATABASE_URL");
    const workers: number = config.require("WORKERS", { type: "integer" });
    const applied: number = config.loadDotenv(".env");
    // @ts-expect-error -- unknown type
    config.get("X", { type: "date" });
    // @ts-expect-error -- an optional setting may be absent
    const maybe: string = config.get("MAYBE");

    const delay: number = retry.backoff(3, { baseMs: 50, jitter: true });
    const status: number = await retry.run(async () => 200, {
        maxAttempts: 4,
        retryOn: (s) => s >= 500,
        onRetry: (attempt, ok) => logx.warn("retry " + attempt + " " + ok),
    });
    // @ts-expect-error -- run resolves asynchronously
    const syncStatus: number = retry.run(() => 1);

    const rl = logx.with({ requestId: "r1", user: 42 });
    rl.info("handled");
    rl.with({ step: 2 }).error(new Error("boom"));
    const frag: string = logx.fields({ a: 1 });
    logx.debug("x" + frag);
    void [port, host, debug, dsn, workers, applied, maybe, delay, status, syncStatus];
}
