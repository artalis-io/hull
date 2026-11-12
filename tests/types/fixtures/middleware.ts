// Fixture: the hull:web:middleware:* modules as an app uses them.
import { app } from "hull:app";
import { cookie } from "hull:web:cookie";
import { auditLog, type AuditEvent } from "hull:web:middleware:audit-log";
import { auth } from "hull:web:middleware:auth";
import { cors } from "hull:web:middleware:cors";
import cspDefault, { csp } from "hull:web:middleware:csp";
import { csrf } from "hull:web:middleware:csrf";
import { etag } from "hull:web:middleware:etag";
import { health } from "hull:web:middleware:health";
import { idempotency } from "hull:web:middleware:idempotency";
import { inbox } from "hull:web:middleware:inbox";
import { logger } from "hull:web:middleware:logger";
import { oauth } from "hull:web:middleware:oauth";
import { outbox } from "hull:web:middleware:outbox";
import { ratelimit } from "hull:web:middleware:ratelimit";
import { rbac } from "hull:web:middleware:rbac";
import { session, type SessionData, type SessionListing } from "hull:web:middleware:session";
import { totp } from "hull:web:middleware:totp";
import { transaction } from "hull:web:middleware:transaction";

export function buildStack(): Hull.Middleware[] {
    const stack: Hull.Middleware[] = [
        cors.middleware({ origins: ["https://app.example.com"], credentials: true, maxAge: 600 }),
        csp.htmx({ reportOnly: true }),
        cspDefault.strict(),
        csrf.middleware({ secret: "x".repeat(32), requireSession: true, cookieName: "sid" }),
        logger.middleware({ skip: ["/health"], includeHeaders: ["User-Agent"] }),
        ratelimit.middleware({ limit: 10, window: 60, key: (req) => req.ctx.session_id ?? "anon" }),
        ratelimit.middleware({ key: "global", trustProxy: true }),
        health.middleware({ details: false, dbCheck: true }),
        idempotency.middleware({ getPrincipal: (req) => req.ctx.session?.user_id ?? "__anon", methods: ["POST", "PATCH"] }),
        outbox.middleware(),
        transaction.middleware(),
        auth.sessionMiddleware({ loginPath: "/login", excludePaths: ["/public/*"] }),
        auth.jwtMiddleware({ secret: "s".repeat(32), aud: "api", leeway: 5 }),
        rbac.requireRole(["admin", "owner"]),
        rbac.requirePermission("todos.manage", { getUserId: (req) => req.ctx.user?.sub }),
    ];
    const allowed: boolean = cors.isAllowedOrigin("https://a.example", ["*"]);
    const n: string | null = csp.nonce();
    void allowed; void n;
    // @ts-expect-error csrf.middleware requires a secret
    csrf.middleware({});
    // @ts-expect-error misspelled option
    cors.middleware({ origin: ["https://x"] });
    // @ts-expect-error jwtMiddleware needs its options
    auth.jwtMiddleware();
    // @ts-expect-error a rate-limit key function takes the request and returns a key
    ratelimit.middleware({ key: (req: Hull.Request) => ({ id: 1 }) });
    return stack;
}

export function handlers(req: Hull.Request, res: Hull.Response): void {
    const nonce: string | undefined = req.ctx.csp_nonce;
    const token: string | undefined = req.ctx.csrf_token;
    const rid: string | undefined = req.ctx.request_id;
    void nonce; void token; void rid;

    etag.json(req, res, { items: [1, 2, 3] });
    etag.text(req, res, "hello", 200);
    etag.html(req, res, "<h1>hi</h1>");
    const tag: string | null = etag.compute("body");
    const hit: boolean = etag.matches(req, tag);
    void hit;
    // @ts-expect-error status is a number
    etag.json(req, res, {}, "200");

    const sid: string = auth.login(req, res, { user_id: 7, email: "a@b.c" }, { ttl: 3600, cookieOpts: { secure: false } });
    auth.logout(req, res, { name: "hull_session" });
    void sid;

    idempotency.respond(req, res, 201, { id: 42 }, { Location: "/items/42" });
    idempotency.respondHtml(req, res, 200, "<p>ok</p>");
    idempotency.complete(req);
    // @ts-expect-error status code comes before the body
    idempotency.respond(req, res, { id: 1 });
}

export function sessions(req: Hull.Request): void {
    session.init({ ttl: 3600, absoluteTtl: false, cleanupAt: "04:00" });
    const id: string = session.create({ user_id: 1, theme: "dark" }, { req });
    const data: SessionData | null = session.load(id);
    if (data) {
        const uid: string | number | undefined = data.user_id;
        void uid;
        session.update(id, { ...data, theme: "light" });
    }
    const list: SessionListing[] = session.listForUser(1, req);
    for (const s of list) {
        if (!s.current) session.destroyRef(1, s.ref);
    }
    const removed: number = session.destroyOthers(id, 1) + session.destroyAll("1") + session.cleanup();
    const newId: string = session.rotate(id, { user_id: 1 });
    const gone: boolean = session.destroy(newId);
    void removed; void gone;
    // @ts-expect-error absoluteTtl is a number or false
    session.init({ absoluteTtl: true });

    const onLogin = session.loginHandler(cookie, {
        auditLog,
        onNewDevice: (_req, _res, user) => { void user.email; },
        respond: (res, user, sid) => res.json({ ok: true, id: user.id ?? null, sid }),
    });
    const onLogout = session.logoutHandler(cookie, { trustedHosts: ["app.example.com"] });
    void onLogin; void onLogout;
    // @ts-expect-error the cookie module is required
    session.loginHandler();
}

export function audit(req: Hull.Request): void {
    auditLog.init({ fingerprintSalt: "deployment-salt", retainDays: 90 });
    auditLog.record(1, "login", req, { metadata: { factors: "password" } });
    const events: AuditEvent[] = auditLog.list(1, { limit: 20, kinds: ["login"] });
    const firstKind: string | undefined = events[0]?.kind;
    const devices = auditLog.listDevices(1, { window_days: 30 });
    const lastSeen: number | undefined = devices[0]?.last_seen;
    const fresh: boolean = auditLog.isNewDevice(1, req);
    const fp: string = auditLog.fingerprint(req);
    const status: "scheduled" | "external" | "missing" = auditLog.cleanupStatus();
    const r = auditLog.recomputeFingerprints();
    void firstKind; void lastSeen; void fresh; void fp; void status; void r.updated;
    // @ts-expect-error the fingerprint salt is required
    auditLog.init({ retainDays: 30 });
}

export async function storage(): Promise<void> {
    idempotency.init({ ttl: 86400 });
    const cleaned: number = idempotency.cleanup();

    inbox.init({ ttl: 604800 });
    const dup: boolean = inbox.checkAndMark("evt_1", "stripe");
    inbox.markProcessed(123, "github", { ttl: 3600 });
    const seen: boolean = inbox.isDuplicate("evt_1");
    void dup; void seen; void cleaned; void inbox.cleanup();
    // @ts-expect-error a message id is a string or a number
    inbox.isDuplicate({ id: 1 });

    outbox.init({ maxAttempts: 5 });
    const rowId: number = outbox.enqueue({
        kind: "webhook",
        destination: "https://hooks.example.com",
        payload: JSON.stringify({ a: 1 }),
        idempotencyKey: "evt-1",
    });
    const result = await outbox.flush({ limit: 10 });
    const delivered: number = result.delivered;
    const busy: true | undefined = result.busy;
    const stats = outbox.stats();
    const pending: number = stats.pending;
    const deleted: number = outbox.cleanup(86400, { alsoFailed: true });
    void rowId; void delivered; void busy; void pending; void deleted;
    // @ts-expect-error flush returns a Promise; await it first
    const bad: number = outbox.flush().delivered;
    void bad;
    // @ts-expect-error payload is required
    outbox.enqueue({ kind: "webhook", destination: "x" });

    const [ok, err] = transaction.attempt(() => { /* writes */ });
    if (!ok) void String(err);
    transaction.run(() => {});
    const t = transaction.try(() => {});
    void t;
}

export function checks(): void {
    health.register("cache", () => true);
    health.unregister("cache");
    const report = health.runChecks({ dbCheck: false });
    const allOk: boolean = report.allOk;
    const s: "ok" | "fail" | undefined = report.checks["db"]?.status;
    void allOk; void s;
    // @ts-expect-error checks are synchronous
    health.register("async", async () => true);

    const store = new Map<string, { count: number; windowStart: number }>();
    const res = ratelimit.check(store, "k", 5, 60, 1_700_000_000);
    const remaining: number = res.remaining;
    void remaining;

    const line: string = logger.formatLine([["method", "GET"], ["status", 200]]);
    const skip: boolean = logger.shouldSkip("/health", ["/health"]);
    const genId: string = logger.generateId();
    void line; void skip; void genId;

    const tok: string = csrf.generate("sid", "secret");
    const valid: boolean = csrf.verify(tok, "sid", "secret", 600);
    void valid;
}

export function access(): void {
    rbac.init();
    rbac.defineRole("admin", ["users.manage"]);
    rbac.definePermission("users.read");
    rbac.assign(1, "admin");
    rbac.grant("admin", "users.read");
    const roles: string[] = rbac.roles("1");
    const perms: string[] = rbac.permissions(1);
    const can: boolean = rbac.hasPermission(1, "users.read") && rbac.hasAnyRole(1, ["admin"])
        && rbac.hasRole(1, "admin") && rbac.hasAnyPermission(1, ["x"]);
    rbac.revoke(1, "admin");
    rbac.ungrant("admin", "users.read");
    void roles; void perms; void can;
    // @ts-expect-error a role name is required
    rbac.requireRole();

    totp.init({ issuer: "Acme", digits: 6, encryptionKey: "k".repeat(32), onLockout: (u, until) => { void u; void until; } });
    const enrol = totp.enroll(1);
    const svg: string = enrol.qrSvg;
    const codes: string[] = enrol.recoveryCodes;
    const confirmed: boolean = totp.confirm(1, "123456");
    const [matched, kind] = totp.verifyWithKind(1, "123456");
    if (matched) { const k: "totp" | "recovery" = kind; void k; }
    const enrolled: boolean = totp.enrolled(1) && totp.verify(1, "123456");
    const st = totp.rekeyStatus();
    const total: number = st.total + totp.rekey().rekeyed + totp.cleanup() + totp.lockoutRemaining(1);
    const off: boolean = totp.disable(1);
    void svg; void codes; void confirmed; void enrolled; void total; void off;
    // @ts-expect-error digits is 6 or 8
    totp.init({ digits: 7 });
}

export function signIn(): void {
    oauth.init({
        secret: "s".repeat(32),
        baseUrl: "https://app.example.com",
        providers: {
            google: { preset: "google", clientId: "id", clientSecret: "secret" },
            entra: { preset: "microsoft", tenant: "common", clientId: "id", scopes: ["openid", "email"] },
        },
        findUser: (provider, claims) => ({ id: String(claims["sub"]), email: provider }),
        onLogin: session.loginHandler(cookie),
        onLogout: session.logoutHandler(cookie),
    });
    oauth.routes(app);
    // @ts-expect-error providers are required
    oauth.init({ secret: "s".repeat(32) });
    // @ts-expect-error unknown preset
    oauth.init({ providers: { x: { preset: "github", clientId: "id" } } });
}
