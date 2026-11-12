// Fixture: the hull:web:* helpers (htmx widgets, cookie, flash, form,
// pagination, pwned, auth-flows, auth-health, attachment-serve) as an app uses them.
import { app } from "hull:app";
import htmxDefault, { htmx } from "hull:web:htmx";
import { confirm } from "hull:web:htmx:confirm";
import { form as htmxForm } from "hull:web:htmx:form";
import { inlineEdit } from "hull:web:htmx:inline-edit";
import { pagination as htmxPagination } from "hull:web:htmx:pagination";
import { search } from "hull:web:htmx:search";
import { sort, type SortState } from "hull:web:htmx:sort";
import { table } from "hull:web:htmx:table";
import { toast } from "hull:web:htmx:toast";
import { cookie } from "hull:web:cookie";
import { flash, type FlashMessage } from "hull:web:flash";
import { form } from "hull:web:form";
import { pagination } from "hull:web:pagination";
import { pwned } from "hull:web:pwned";
import { authFlows, type AuthFlowsUser } from "hull:web:auth-flows";
import { authHealth } from "hull:web:auth-health";
import { attachmentServe } from "hull:web:attachment-serve";

export function htmxHandler(req: Hull.Request, res: Hull.Response): void {
    const isHx: boolean = htmx.is(req) && !htmxDefault.boosted(req);
    const from: string | null = htmx.currentUrl(req);
    const tgt: string | null = htmx.target(req);
    const name: string | null = htmx.triggerName(req);
    const safe: string = htmx.escape(from ?? tgt ?? name ?? 42);
    htmx.retarget(res, "#main");
    htmx.reswap(res, "outerHTML");
    htmx.trigger(res, "saved");
    htmx.trigger(res, "saved", { id: 1 }, { timing: "swap" });
    htmx.trigger(res, { saved: { id: 1 }, refreshList: null }, { timing: "settle" });
    htmx.pushUrl(res, "/todos?page=2");
    htmx.replaceUrl(res, false);
    htmx.location(res, { path: "/todos", target: "#main", swap: "innerHTML" });
    htmx.location(res, "//cdn.example.com/x", { allowProtocolRelative: true });
    if (isHx) htmx.refresh(res);
    htmx.redirect(req, res, "/done" + safe);
    // @ts-expect-error: a timing that does not exist
    htmx.trigger(res, "saved", null, { timing: "later" });
    // @ts-expect-error: pushUrl takes a string or false, not true
    htmx.pushUrl(res, true);
    // @ts-expect-error: redirect needs the request first
    htmx.redirect(res, "/done");
}

export function widgets(req: Hull.Request, res: Hull.Response): string {
    const c: string = confirm.attrs("Delete this item?", { yes: "Delete", danger: true, title: "Careful" });
    const errs = { email: "is required" };
    const f: string = htmxForm.errors(errs) + htmxForm.fieldError(errs, "email")
        + htmxForm.fieldAttrs(null, "email");
    const view: string = inlineEdit.cell({ value: "Alice", editUrl: "/users/1/name/edit" });
    const editor: string = inlineEdit.editor({
        value: "Alice", saveUrl: "/users/1/name", cancelUrl: "/users/1/name", saveLabel: "OK",
    });
    const nav: string = htmxPagination.nav(250, { page: 2, perPage: 25, baseUrl: "/rows", target: "#grid" });
    const input: string = search.inputAttrs({ url: "/assets/search", delayMs: 200, method: "POST" });
    const results: string = search.resultsAttrs({ id: "out" });
    const current: SortState | null = sort.parse(req, { allowed: ["name", "created_at"], default: "name" });
    if (current) {
        const dir: "asc" | "desc" = current.direction;
        void dir;
    }
    const th: string = sort.headerAttrs("name", current, { url: "/rows", swap: "innerHTML" });
    const rows = [{ id: 1, name: "Alice", age: 30 }];
    const grid: string = table.render(rows, [
        { name: "name", label: "Name", sortable: true, editable: true },
        { name: "age", render: (v, row) => String(v) + " (" + row.name + ")" },
    ], {
        baseUrl: "/rows", currentSort: current,
        editUrlFor: (row, col) => "/rows/" + row.id + "/" + col + "/edit",
    });
    toast.success(res, "Saved");
    toast.show(res, "Heads up", { level: "warning", duration: 0, id: "h1" });
    toast.error(res, "Denied", { duration: 8000 });
    // @ts-expect-error: confirm.attrs needs the question
    confirm.attrs();
    // @ts-expect-error: inline-edit cell needs editUrl
    inlineEdit.cell({ value: "x" });
    // @ts-expect-error: search method is get or post
    search.inputAttrs({ url: "/s", method: "put" });
    // @ts-expect-error: sort.parse requires the allowlist
    sort.parse(req, { default: "name" });
    // @ts-expect-error: table.render needs baseUrl
    table.render(rows, [], {});
    // @ts-expect-error: an unknown toast level
    toast.show(res, "x", { level: "fatal" });
    // @ts-expect-error: the shorthands fix the level
    toast.info(res, "x", { level: "error" });
    return c + f + view + editor + nav + input + results + th + grid;
}

export function cookies(req: Hull.Request, res: Hull.Response, header: string | undefined): void {
    const jar: { [name: string]: string } = cookie.parse(header);
    const sid: string | undefined = jar.session;
    const set: string = cookie.serialize("session", sid ?? "", {
        path: "/", httpOnly: true, sameSite: "Strict", maxAge: 3600, secure: false,
    });
    const del: string = cookie.clear("session", { path: "/" });
    const fields: { [name: string]: string } = form.parse("a=1&b=2", { maxFields: 10 });
    flash.set(req, "Saved.", "success");
    const msgs: FlashMessage[] = flash.consume(req);
    const first: string = msgs.length > 0 ? msgs[0].text + msgs[0].kind : "";
    flash.trigger(res, "Done");
    void [set, del, fields.a, first];
    // @ts-expect-error: sameSite is Lax / Strict / None
    cookie.serialize("a", "b", { sameSite: "lax" });
    // @ts-expect-error: a misspelled option
    cookie.serialize("a", "b", { httponly: false });
    // @ts-expect-error: maxFields is a number
    form.parse("a=1", { maxFields: "10" });
}

export function pages(req: Hull.Request): number {
    const p = pagination.fromQuery(req, { defaultPerPage: 25, maxPerPage: 50 });
    const nav = pagination.render(500, { page: p.page, per_page: p.per_page, baseUrl: "/todos" });
    let n = p.limit + p.offset;
    for (const link of nav.links) {
        if (link.ellipsis) continue;
        if (link.active) n += link.page;
        const url: string = link.url;
        void url;
    }
    const prev: string | null = nav.prev;
    void prev;
    // @ts-expect-error: render's per-page option is per_page, not perPage
    pagination.render(10, { perPage: 5 });
    return n + (nav.show ? nav.pages : 0);
}

export async function passwords(pw: string): Promise<boolean> {
    const breached: boolean = await pwned.check(pw, { endpoint: "https://api.pwnedpasswords.com/range/" });
    const h = pwned.health();
    const ok: boolean | null = h.ok;
    void ok;
    // @ts-expect-error: check returns a Promise; it must be awaited
    const bad: boolean = pwned.check(pw);
    void bad;
    return breached;
}

export function setupAuth(sessionLogin: (req: Hull.Request, res: Hull.Response, user: unknown) => unknown): void {
    const users = authFlows.standardUsers({ table: "users" });
    authFlows.init({
        secret: "0123456789abcdef0123456789abcdef",
        publicOrigin: "https://app.example.com",
        emailSend: (to, subject, html, text) => { void [to, subject, html, text]; },
        templates: {
            welcome: (c) => ({ subject: "Welcome", text: "verify: " + c.verify_url }),
            magic_link: (c) => ({ subject: "Sign in", html: "<a href='" + c.link + "'>go</a>" }),
            password_reset: (c) => ({ subject: "Reset", text: c.link }),
            email_change: (c) => ({ subject: "Confirm", text: (c.new_email ?? "") + c.link }),
        },
        users,
        onLogin: (req, res, user, ctx) => {
            const factors: string = ctx.factors;
            void factors;
            return sessionLogin(req, res, user);
        },
        onPasswordReset: async (_req, _res, user) => { void user.id; },
        loginRatelimit: { limit: 10, window: 60 },
        emailRateLimit: false,
        enableTotp: true,
        userTotpEnrolled: (uid) => uid !== "",
        totpVerify: (user: AuthFlowsUser, code) => code === "123456" && user.id !== undefined,
        totpDisable: (uid) => { void uid; },
        cleanupAt: "04:30",
    });
    authFlows.routes(app);
    authFlows.sendMagicLink("alice@example.com", "https://app.example.com");
    const reserved: boolean = authFlows.emailReserved("alice@example.com");
    const unlocked: boolean = authFlows.unlockRecovery("u1");
    authFlows.cleanup();
    void [reserved, unlocked];
    // @ts-expect-error: init needs emailSend, templates and onLogin
    authFlows.init({ secret: "0123456789abcdef0123456789abcdef" });
    authFlows.init({
        secret: "0123456789abcdef0123456789abcdef",
        emailSend: () => {},
        templates: {},
        onLogin: () => {},
        // @ts-expect-error: emailRateLimit is false or { limit, window }
        emailRateLimit: true,
    });
}

export function health(): boolean {
    authHealth.routes(app, { authCheck: (req) => req !== null, includeCounts: false });
    const report = authHealth.check({ includeCounts: true });
    const pw = report.pwned;
    if ("status" in pw && pw.status === "fail_open") return false;
    // @ts-expect-error: routes requires the authCheck gate
    authHealth.routes(app, {});
    return report.all_ok;
}

export function files(req: Hull.Request, res: Hull.Response, id: string): Promise<void> {
    // @ts-expect-error: the id is a string
    attachmentServe.serve(req, res, 42);
    return attachmentServe.serve(req, res, id, {
        authCheck: async (_req, meta) => meta.uploaded_by === "u1" && meta.mime.startsWith("image/"),
    });
}
