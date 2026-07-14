// Todo App — full auth, CSRF, rate limiting, server-side rendered, i18n
//
// A todo list with user authentication, sessions, CSRF protection,
// per-user data isolation, and English/Hungarian language support.
// Pure HTML forms, no client-side JS.
//
// Graceful shutdown: Ctrl+C triggers drain mode (finishes in-flight
// requests within 5s). Second Ctrl+C stops immediately.
//
// Run:  hull dev examples/todo/app.js -d /tmp/todo.db

import { app } from "hull:app";
import { crypto } from "hull:crypto";
import { csv } from "hull:csv";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { httpClient } from "hull:http-client";
import { i18n } from "hull:i18n";
import { log } from "hull:log";
import { search } from "hull:search";
import { template } from "hull:template";
import { time } from "hull:time";
import { validate } from "hull:validate";
import { cookie } from "hull:web:cookie";
import { form } from "hull:web:form";
import { auth } from "hull:web:middleware:auth";
import { csrf } from "hull:web:middleware:csrf";
import { logger } from "hull:web:middleware:logger";
import { ratelimit } from "hull:web:middleware:ratelimit";
import { rbac } from "hull:web:middleware:rbac";
import { session } from "hull:web:middleware:session";
import en from "./locales/en.json";
import hu from "./locales/hu.json";

app.manifest({
    hosts: ["127.0.0.1"],  // allow self-fetch for /api/stats
    modules: [
        "hull/http-server@1",
        "hull/log@1",
        "hull/web/cookie@1",
        "hull/crypto@1",
        "hull/csv@1",
        "hull/db@1",
        "hull/web/form@1",
        "hull/http-client@1",
        "hull/i18n@1",
        "hull/search@1",
        "hull/template@1",
        "hull/time@1",
        "hull/validate@1",
        "hull/web/middleware/auth@1",
        "hull/web/middleware/csrf@1",
        "hull/web/middleware/logger@1",
        "hull/web/middleware/ratelimit@1",
        "hull/web/middleware/rbac@1",
        "hull/web/middleware/session@1",
    ],
});

// ── i18n setup ─────────────────────────────────────────────────────

i18n.load("en", en);
i18n.load("hu", hu);
i18n.locale("en");

// ── Session setup ──────────────────────────────────────────────────

session.init({ ttl: 3600 });

// ── RBAC setup ────────────────────────────────────────────────────

rbac.init();
rbac.defineRole("user", ["todos.manage"]);
rbac.defineRole("admin", ["todos.manage", "admin.dashboard"]);

// ── Search index ──────────────────────────────────────────────────

search.createIndex("todos", ["title"]);
search.reindex("todos", "todos", { columns: { title: "title" }, idColumn: "id" });

// ── CSRF secret ─────────────────────────────────────────────────────

function toHex(buf) {
    const bytes = new Uint8Array(buf);
    let hex = "";
    for (let i = 0; i < bytes.length; i++)
        hex += bytes[i].toString(16).padStart(2, "0");
    return hex;
}
const csrfSecret = toHex(crypto.random(32));

// ── Middleware stack ─────────────────────────────────────────────────

app.use("*", "/*", logger.middleware({ skip: ["/health"] }));

// Optional session loading
app.use("*", "/*", (req, _res) => {
    const header = req.headers.cookie;
    if (!header) return 0;
    const cookies = cookie.parse(header);
    const sessionId = cookies.hull_session;
    if (sessionId) {
        const data = session.load(sessionId);
        if (data) {
            if (!req.ctx) req.ctx = {};
            req.ctx.session = data;
            req.ctx.session_id = sessionId;
        }
    }
    return 0;
});

// Language detection middleware: cookie → Accept-Language → default "en"
app.use("*", "/*", (req, _res) => {
    const cookies = cookie.parse(req.headers.cookie || "");
    let lang = cookies["hull.lang"];
    if (!lang || (lang !== "en" && lang !== "hu")) {
        lang = i18n.detect(req.headers["accept-language"]) || "en";
    }
    i18n.locale(lang);
    return 0;
});

app.use("POST", "/login", ratelimit.middleware({ limit: 10, window: 60 }));
app.use("POST", "/register", ratelimit.middleware({ limit: 5, window: 60 }));
// CSRF needs body access → post-body middleware
app.usePost("*", "/*", csrf.middleware({ secret: csrfSecret }));

// ── Helpers ─────────────────────────────────────────────────────────

function requireSession(req, res) {
    if (!req.ctx || !req.ctx.session) {
        res.redirect("/login");
        return null;
    }
    return req.ctx.session;
}

function render(page, req, extra) {
    const ctx = Object.assign({}, extra || {});
    ctx.year = new Date().getFullYear().toString();
    ctx.csrf_token = req.ctx?.csrf_token ?? "";
    ctx.user = req.ctx?.session ?? null;
    ctx.logged_in = !!req.ctx?.session;
    ctx.lang = i18n.locale();
    ctx.is_admin = false;
    if (req.ctx?.session) {
        ctx.is_admin = rbac.hasRole(String(req.ctx.session.user_id), "admin");
    }

    // Inject translated strings as t.* for templates
    ctx.t = {
        site_title:    i18n.t("site.title"),
        powered_by:    i18n.t("site.powered_by"),
        nav_brand:     i18n.t("nav.brand"),
        nav_tasks:     i18n.t("nav.tasks"),
        nav_logout:    i18n.t("nav.logout"),
        nav_login:     i18n.t("nav.login"),
        nav_register:  i18n.t("nav.register"),
        my_todos:      i18n.t("index.title"),
        placeholder:   i18n.t("index.placeholder"),
        add:           i18n.t("index.add"),
        remaining:     i18n.t("index.remaining"),
        completed:     i18n.t("index.completed"),
        total:         i18n.t("index.total"),
        empty:         i18n.t("index.empty"),
        login_title:   i18n.t("login.page_title"),
        login_email:   i18n.t("login.email"),
        login_pass:    i18n.t("login.password"),
        login_submit:  i18n.t("login.submit"),
        no_account:    i18n.t("login.no_account"),
        register_link: i18n.t("login.register_link"),
        reg_title:     i18n.t("register.page_title"),
        reg_name:      i18n.t("register.name"),
        reg_email:     i18n.t("register.email"),
        reg_pass:      i18n.t("register.password"),
        reg_submit:    i18n.t("register.submit"),
        has_account:   i18n.t("register.has_account"),
        login_link:    i18n.t("register.login_link"),
        lang_en:       i18n.t("lang.en"),
        lang_hu:       i18n.t("lang.hu"),
        // search & export
        search_placeholder: i18n.t("index.search_placeholder"),
        search_btn:    i18n.t("index.search"),
        clear_search:  i18n.t("index.clear_search"),
        export_csv:    i18n.t("index.export_csv"),
        // admin
        nav_admin:     i18n.t("nav.admin"),
        admin_title:   i18n.t("admin.title"),
        admin_name:    i18n.t("admin.name"),
        admin_email:   i18n.t("admin.email"),
        admin_todos:   i18n.t("admin.todos"),
        admin_role:    i18n.t("admin.role"),
        admin_joined:  i18n.t("admin.joined"),
        admin_no_users: i18n.t("admin.no_users"),
        admin_total_users: i18n.t("admin.total_users"),
        no_results:    i18n.t("index.no_results"),
    };

    return template.render(page, ctx);
}

// ── Health check ────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// Stats endpoint: uses httpClient.async.get() to check own health
app.get("/api/stats", async (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const host = req.headers.host || "127.0.0.1:3000";
    const port = host.split(":")[1] || "3000";
    const health = await httpClient.async.get(`http://127.0.0.1:${port}/health`);
    const count = db.query("SELECT COUNT(*) as n FROM todos WHERE user_id = ?",
                           [sess.user_id]);
    res.json({
        todo_count: count[0].n,
        server_healthy: health.status === 200,
    });
});

// ── Language switch ─────────────────────────────────────────────────

app.get("/lang/:code", (req, res) => {
    let code = req.params.code;
    if (code !== "en" && code !== "hu") code = "en";
    // secure: false lets `hull dev` (plain HTTP on :8080) work in a
    // real browser. Production over HTTPS should flip to true (or
    // remove the opt; default is true) so the cookie is HTTPS-only.
    res.header("Set-Cookie", cookie.serialize("hull.lang", code, {
        path: "/", maxAge: 365 * 24 * 3600, httpOnly: false, secure: false,
    }));
    const referer = req.headers.referer;
    const target = referer?.startsWith("/") ? referer : "/";
    res.redirect(target);
});

// ── Auth routes ─────────────────────────────────────────────────────

app.get("/login", (req, res) => {
    if (req.ctx?.session) {
        return res.redirect("/");
    }
    res.html(render("pages/login.html", req, { error: null }));
});

app.get("/register", (req, res) => {
    if (req.ctx?.session) {
        return res.redirect("/");
    }
    res.html(render("pages/register.html", req, { error: null }));
});

app.post("/login", (req, res) => {
    const params = form.parse(req.body);
    const [ok, errors] = validate.check(params, {
        email:    { required: true },
        password: { required: true },
    });
    if (!ok) {
        return res.html(render("pages/login.html", req, { error: errors.email || errors.password }));
    }

    const { email, password } = params;

    const rows = db.query("SELECT * FROM users WHERE email = ?", [email]);
    if (rows.length === 0) {
        return res.html(render("pages/login.html", req, { error: "Invalid credentials" }));
    }

    const user = rows[0];
    if (!crypto.verifyPassword(password, user.password_hash)) {
        return res.html(render("pages/login.html", req, { error: "Invalid credentials" }));
    }

    auth.login(req, res, { user_id: user.id, email: user.email, name: user.name });
    res.redirect("/");
});

app.post("/register", (req, res) => {
    const params = form.parse(req.body);
    const [ok, errors] = validate.check(params, {
        email:    { required: true },
        password: { required: true, min: 8 },
        name:     { required: true },
    });
    if (!ok) {
        return res.html(render("pages/register.html", req, { error: errors.email || errors.password || errors.name }));
    }

    const { email, password, name } = params;

    const existing = db.query("SELECT id FROM users WHERE email = ?", [email]);
    if (existing.length > 0) {
        return res.html(render("pages/register.html", req, { error: "Email already registered" }));
    }

    const hash = crypto.hashPassword(password);
    db.exec("INSERT INTO users (email, password_hash, name, created_at) VALUES (?, ?, ?, ?)",
            [email, hash, name, time.now()]);
    const userId = db.lastId();

    // Assign RBAC roles (first user gets admin)
    rbac.assign(String(userId), "user");
    const userCount = db.query("SELECT COUNT(*) as cnt FROM users");
    if (userCount[0].cnt === 1) {
        rbac.assign(String(userId), "admin");
    }

    auth.login(req, res, { user_id: userId, email: email, name: name });
    res.redirect("/");
});

app.post("/logout", (req, res) => {
    auth.logout(req, res);
    res.redirect("/login");
});

// ── Todo routes (authenticated) ─────────────────────────────────────

app.get("/", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const q = req.query?.q || null;
    let todos;

    if (q && q !== "") {
        let results;
        try {
            results = search.query("todos", q, { limit: 100 });
        } catch (_e) {
            results = [];
        }
        if (results.length > 0) {
            const ids = results.map(r => r.id);
            const placeholders = ids.map(() => "?").join(",");
            const params = [...ids, sess.user_id];
            todos = db.query(
                `SELECT * FROM todos WHERE id IN (${placeholders}) AND user_id = ? ORDER BY created_at DESC`, params);
        } else {
            todos = [];
        }
    } else {
        todos = db.query(
            "SELECT * FROM todos WHERE user_id = ? ORDER BY created_at DESC",
            [sess.user_id]);
    }

    let doneCount = 0;
    for (let i = 0; i < todos.length; i++) {
        todos[i].done = (todos[i].done === 1);
        if (todos[i].done) doneCount++;
    }

    res.html(render("pages/index.html", req, {
        todos: todos,
        has_todos: todos.length > 0,
        total: todos.length,
        done_count: doneCount,
        remaining: todos.length - doneCount,
        search_query: q || "",
        searching: q != null && q !== "",
    }));
});

app.post("/add", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const params = form.parse(req.body);
    const [titleOk] = validate.check(params, {
        title: { required: true },
    });
    if (!titleOk) {
        return res.redirect("/");
    }

    let title = params.title;
    if (title.length > 500) title = title.substring(0, 500);

    db.exec("INSERT INTO todos (user_id, title, created_at) VALUES (?, ?, ?)",
            [sess.user_id, title, time.now()]);
    const todoId = db.lastId();
    search.index("todos", todoId, { title: title });
    res.redirect("/");
});

app.post("/toggle/:id", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    db.exec(
        "UPDATE todos SET done = CASE WHEN done = 0 THEN 1 ELSE 0 END WHERE id = ? AND user_id = ?",
        [Number.parseInt(req.params.id), sess.user_id]);
    res.redirect("/");
});

app.post("/delete/:id", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const todoId = Number.parseInt(req.params.id);
    db.exec("DELETE FROM todos WHERE id = ? AND user_id = ?",
            [todoId, sess.user_id]);
    search.remove("todos", todoId);
    res.redirect("/");
});

// ── CSV export ────────────────────────────────────────────────────

app.get("/export", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const todos = db.query(
        "SELECT title, done, created_at FROM todos WHERE user_id = ? ORDER BY created_at DESC",
        [sess.user_id]);

    for (let i = 0; i < todos.length; i++) {
        todos[i].done = todos[i].done === 1 ? "yes" : "no";
    }

    const out = csv.encode(todos, { headers: true });
    res.header("Content-Disposition", "attachment; filename=\"todos.csv\"");
    res.text(out);
});

// ── Admin dashboard (RBAC-protected) ──────────────────────────────

app.get("/admin", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    if (!rbac.hasRole(String(sess.user_id), "admin")) {
        res.status(403);
        res.json({ error: "forbidden" });
        return;
    }

    const users = db.query(
        "SELECT u.id, u.name, u.email, u.created_at, " +
        "COUNT(t.id) as todo_count " +
        "FROM users u " +
        "LEFT JOIN todos t ON t.user_id = u.id " +
        "GROUP BY u.id " +
        "ORDER BY u.created_at DESC"
    );

    for (let i = 0; i < users.length; i++) {
        const roles = rbac.roles(String(users[i].id));
        users[i].role = roles.length > 0 ? roles.join(", ") : "none";
    }

    res.html(render("pages/admin.html", req, {
        users: users,
        has_users: users.length > 0,
        user_count: users.length,
    }));
});

log.info("Todo app loaded — routes registered (en/hu i18n, csv, search, rbac)");
