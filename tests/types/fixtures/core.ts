// Fixture: the app intrinsic, the core shapes, the globals, the test harness and
// the core modules, used as an app would.
import { app } from "hull:app";
import type { Router } from "hull:app";
import { httpServer } from "hull:http-server";
import { wsServer } from "hull:web:ws-server";
import { wsClient } from "hull:web:ws-client";
import { log } from "hull:log";
import { json } from "hull:json";
import { env } from "hull:env";
import { time } from "hull:time";
import { fs } from "hull:fs";
import type { FsError, FsStat } from "hull:fs";
import { worker } from "hull:worker";

export function manifest(): void {
    app.manifest({
        modules: ["hull/http-server@1", "hull/web/ws-server@1", "hull/web/sse@1",
                  "hull/timers@1", "hull/gpu@1?", "hull/db@1"],
        fs: { read: ["data"], write: ["data/out", "@state/cache"] },
        env: ["PORT"],
        secrets: ["DATABASE_URL"],
        hosts: ["api.example.com", "*.internal", "10.0.0.0/8"],
        http: { timeoutMs: 5000 },
        csp: false,
        cors: { origins: ["https://a.example"], methods: "GET, POST", credentials: true, maxAge: 600 },
        databases: {
            named: { default: "app.db", primary: "$DATABASE_URL",
                     locks: { dsn: "$DATABASE_URL", sessionControl: true } },
            internal: "@state/internal.db",
            dynamic: { hosts: ["10.0.0.0/8"], schemes: ["postgres"] },
        },
        kv: { dynamic: { hosts: ["cache.internal"], schemes: ["valkey"] } },
        ssh: { connect: { hosts: ["*.example.com"], users: ["deploy"], ports: [22] },
               tunnel: { hosts: ["bastion.example.com"] } },
        state: { name: "myapp" },
        tui: false,
        gpu: { devices: [0], dynamicShaders: false },
        wasm: { heap: 1 << 20, timeoutMs: 1000 },
        limits: { requestMs: 2000, perIpConcurrency: 8, trustedProxies: ["127.0.0.1"],
                  routeConcurrency: { "/upload": 2 } },
    });
    // @ts-expect-error - not a registry module
    app.manifest({ modules: ["hull/dbx@1"] });
    // @ts-expect-error - a spec needs its major version
    app.manifest({ modules: ["hull/db"] });
    // @ts-expect-error - misspelled section
    app.manifest({ host: ["a.example"] });
    // @ts-expect-error - csp is a string or false
    app.manifest({ csp: true });
    const m = app.getManifest();
    const mods = m && m.modules;
    void mods;
}

export function routes(): void {
    app.get("/users/:id", (req, res) => {
        const id: string = req.params.id;
        const limit: string | undefined = req.query.limit;
        const ua: string | undefined = req.header("User-Agent");
        const ip: string | undefined = req.remote_addr;
        const body: string | null = req.body;
        req.ctx.user = { id };
        res.status(200).header("X-Id", id).json({ id, limit, ua, ip, body });
    });
    app.post("/raw", async (req, res) => {
        await hull.sleep(5);
        if (req.bodyBytes) res.bytes(req.bodyBytes);
        else res.text(req.body ?? "");
    });
    app.post("/upload", async (req, res) => {
        let total = 0;
        for await (const part of req.multipart()) {
            const name: string = part.name;
            if (part.filename !== null) {
                for await (const chunk of part.chunks()) total += chunk.byteLength;
            } else {
                const value: ArrayBuffer = await part.read();
                total += value.byteLength + name.length;
            }
        }
        res.json({ total }, 201);
    }, { multipart: { maxPartSize: 1 << 20, maxParts: 8 } });
    app.del("/users/:id", (_req, res) => res.redirect("/users", 303));
    app.use("*", "/*", (req, res) => {
        if (!req.headers["authorization"]) { res.status(401).html("<p>no</p>"); return 1; }
        return 0;
    });
    app.usePost("*", "/*", () => 0);
    const api: Router = app.router("/api").get("/ping", (_q, s) => s.text("pong"))
        .use((_q, _s) => 0).router("/v2").post("/x", (_q, s) => s.json(null));
    void api;
    app.ws("/chat", {
        onOpen(conn) { conn.data.joined = time.now(); conn.send("hi " + conn.id); },
        onMessage(conn, msg, isBinary) {
            if (isBinary && msg instanceof ArrayBuffer) conn.sendBinary(msg);
            else wsServer.broadcast(conn.path, String(msg));
        },
        onClose(_conn, code, reason) { log.info("closed", code, reason ?? ""); },
    });
    app.sse("/events", async (_req, stream) => {
        stream.event("tick", json.encode({ n: 1 }), "1");
        stream.event(null, "plain");
        stream.comment("keep-alive");
        stream.close();
    });
    app.every(1000, () => log.debug("tick"));
    app.daily("03:30", async () => { await hull.sleep(1); }, { localtime: true });
    const n: number = wsServer.connections("/chat");
    const stats = httpServer.stats();
    const paused: boolean = stats.listenPaused;
    void n; void paused;

    app.post("/form", async (req, res) => {
        // @ts-expect-error - req.multipart() exists only on a route registered with { multipart }
        for await (const part of req.multipart()) void part;
        res.text("ok");
    });
    // @ts-expect-error - a handler is required
    app.get("/x");
    // @ts-expect-error - misspelled multipart cap
    app.post("/u", () => {}, { multipart: { maxPartsize: 1 } });
    // @ts-expect-error - middleware is synchronous: an async one is refused
    app.use("*", "/*", async () => 0);
    // @ts-expect-error - res.status takes a number
    app.get("/y", (_q, res) => res.status("200"));
    // @ts-expect-error - app.ws takes a handlers object, not a function
    app.ws("/w", () => {});
}

export function mainAndModules(): void {
    app.main(async (ctx) => {
        const first: string | undefined = ctx.args[0];
        const line: string | null = ctx.stdin.read();
        const all: string = ctx.stdin.read("a");
        const some: string | null = ctx.stdin.read(16);
        ctx.stdout.write("line: ", line ?? "", "\n", new Uint8Array([10]));
        ctx.stderr.flush();
        ctx.signal.onabort = (reason) => log.warn("stopping on", reason);
        const port = env.get("PORT") ?? ctx.env.PORT ?? "8080";
        const declared: boolean = env.allowed("PORT");
        const stamp: string = time.datetime() + time.date();
        const ms: number = time.nowMs() - time.clock() + time.now();

        const buf: ArrayBuffer = fs.read("data/in.txt");
        fs.write("data/out/x.txt", buf, { private: true });
        fs.write("data/out/y.txt", "text", { mode: "640" });
        const st: FsStat | null = fs.stat("data/in.txt");
        const size: number = st ? st.size : 0;
        for (const e of fs.list("data")) log.info(e.name, e.type, e.size);
        const map = fs.mmap("data/in.txt", { offset: 0, length: 4096 });
        const len: number = map.length;
        map.close();
        try { fs.read("missing"); } catch (e) { const code: string = (e as FsError).code; void code; }

        const r = await worker.dispatch((c) => c.a * 2, { a: 21 });
        if (typeof r === "object" && r !== null && "error" in r) log.error(r.error);
        else { const doubled: number = r; void doubled; }
        const nothing = await worker.dispatch(() => {});
        const isNull: null | { error: string } = nothing;

        const results: number[] = await hull.map([1, 2, 3], async (x) => x + 1, { limit: 2 });
        const decoded = json.decode("[1]");
        const client = wsClient.connect("wss://api.example.com/feed", {
            onOpen(c) { c.send("hello"); },
            onMessage(_c, msg) { console.log(typeof msg === "string" ? msg : msg.byteLength); },
            onError(_c, err) { console.error(err); },
        });
        client.close(1000, "bye");
        await ctx.signal.promise;
        void [first, all, some, port, declared, stamp, ms, size, len, isNull, results, decoded];

        // @ts-expect-error - fs.write mode is an octal string, not a number
        fs.write("data/out/z", "x", { mode: 0o600 });
        // @ts-expect-error - the worker context is flat scalars
        worker.dispatch(() => 1, { nested: { a: 1 } });
        // @ts-expect-error - read() takes "l", "a" or a count
        ctx.stdin.read("x");
        // @ts-expect-error - hull.sleep returns a Promise: not a number
        const slept: number = hull.sleep(1);
        // @ts-expect-error - env.get may return null
        const strict: string = env.get("PORT");
        void slept; void strict;
        return 0;
    });
    // @ts-expect-error - main returns an exit code, not a string
    app.main(() => "done");
}

export function testFile(): void {
    test("routes", () => {
        const r = test.get("/users/7", { headers: { "X-Y": "z" }, middleware: true });
        test.eq(r.status, 200);
        test.ok(r.json !== undefined, "json body");
        const ct: string | undefined = r.headers?.["content-type"];
        test.err(() => { throw new Error("boom"); }, "boom");
        const typed = test.get<{ id: string }>("/users/7");
        const id: string | undefined = typed.json?.id;
        void ct; void id;
    });
    test("main", { timeout: 1000 }, async () => {
        const out = test.runMain(["a"], { stdin: "input", env: { PORT: "1" }, timeoutMs: 500 });
        test.eq(out.code, 0);
        const bytes: ArrayBuffer = out.stdoutBytes;
        void bytes;
        await hull.sleep(1);
    });
    // @ts-expect-error - body is a string
    test.post("/x", { body: { a: 1 } });
    // @ts-expect-error - a test needs a body function
    test("missing");
}
