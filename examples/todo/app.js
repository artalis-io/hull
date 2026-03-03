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
import { cookie } from "hull:cookie";
import { crypto } from "hull:crypto";
import { db } from "hull:db";
import { form } from "hull:form";
import { i18n } from "hull:i18n";
import { log } from "hull:log";
import { auth } from "hull:middleware:auth";
import { csrf } from "hull:middleware:csrf";
import { logger } from "hull:middleware:logger";
import { ratelimit } from "hull:middleware:ratelimit";
import { session } from "hull:middleware:session";
import { template } from "hull:template";
import { time } from "hull:time";
import { validate } from "hull:validate";

app.manifest({});  // sandbox: no fs, no env, no outbound HTTP; default CSP

// ── i18n setup ─────────────────────────────────────────────────────

i18n.load("en", {
    format: {
        decimalSep: ".", thousandsSep: ",", datePattern: "YYYY-MM-DD",
    },
    site: { title: "Todo", powered_by: "Powered by Hull" },
    nav: {
        brand: "Todo", tasks: "Tasks", logout: "Logout",
        login: "Login", register: "Register",
    },
    index: {
        title: "My Todos", placeholder: "What needs to be done?",
        add: "Add", remaining: "remaining", completed: "completed",
        total: "total", empty: "No todos yet. Add one above!",
    },
    login: {
        title: "Login", page_title: "Login", email: "Email",
        password: "Password", submit: "Login",
        no_account: "Don't have an account?", register_link: "Register",
    },
    register: {
        title: "Register", page_title: "Register", name: "Name",
        email: "Email", password: "Password", submit: "Register",
        has_account: "Already have an account?", login_link: "Login",
    },
    lang: { en: "English", hu: "Magyar" },
});

i18n.load("hu", {
    format: {
        decimalSep: ",", thousandsSep: " ", datePattern: "YYYY.MM.DD.",
    },
    site: { title: "Teend\u0151k", powered_by: "Hull hajtja" },
    nav: {
        brand: "Teend\u0151k", tasks: "Feladatok",
        logout: "Kijelentkez\u00e9s", login: "Bejelentkez\u00e9s",
        register: "Regisztr\u00e1ci\u00f3",
    },
    index: {
        title: "Teend\u0151im",
        placeholder: "Mi a k\u00f6vetkez\u0151 teend\u0151?",
        add: "Hozz\u00e1ad\u00e1s", remaining: "h\u00e1tral\u00e9v\u0151",
        completed: "befejezett", total: "\u00f6sszesen",
        empty: "M\u00e9g nincs teend\u0151. Adj hozz\u00e1 egyet!",
    },
    login: {
        title: "Bejelentkez\u00e9s", page_title: "Bejelentkez\u00e9s",
        email: "E-mail", password: "Jelsz\u00f3",
        submit: "Bel\u00e9p\u00e9s",
        no_account: "Nincs m\u00e9g fi\u00f3kod?",
        register_link: "Regisztr\u00e1ci\u00f3",
    },
    register: {
        title: "Regisztr\u00e1ci\u00f3", page_title: "Regisztr\u00e1ci\u00f3",
        name: "N\u00e9v", email: "E-mail", password: "Jelsz\u00f3",
        submit: "Regisztr\u00e1ci\u00f3",
        has_account: "M\u00e1r van fi\u00f3kod?",
        login_link: "Bejelentkez\u00e9s",
    },
    lang: { en: "English", hu: "Magyar" },
});

i18n.locale("en");

// ── Session setup ──────────────────────────────────────────────────

session.init({ ttl: 3600 });

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
    const sessionId = cookies["hull.sid"];
    if (sessionId) {
        const data = session.load(sessionId);
        if (data) {
            if (!req.ctx) req.ctx = {};
            req.ctx.session = data;
            req.ctx.sessionId = sessionId;
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
    };

    return template.render(page, ctx);
}

// ── Health check ────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// ── Language switch ─────────────────────────────────────────────────

app.get("/lang/:code", (req, res) => {
    let code = req.params.code;
    if (code !== "en" && code !== "hu") code = "en";
    res.header("Set-Cookie", cookie.serialize("hull.lang", code, {
        path: "/", maxAge: 365 * 24 * 3600, httpOnly: false,
    }));
    const referer = req.headers.referer;
    res.redirect(referer || "/");
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

    const todos = db.query(
        "SELECT * FROM todos WHERE user_id = ? ORDER BY created_at DESC",
        [sess.user_id]);

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

    db.exec("DELETE FROM todos WHERE id = ? AND user_id = ?",
            [Number.parseInt(req.params.id), sess.user_id]);
    res.redirect("/");
});

log.info("Todo app loaded — routes registered (en/hu i18n)");
