// HTMX + Pico hypermedia app scaffold.
// Returns full pages for plain navigation; returns fragments when
// HX-Request is set. CSRF + per-request CSP nonce wired in by default.
import { app }        from "hull:app";
import { htmx }       from "hull:web:htmx";
import { flash }      from "hull:web:flash";
import { pagination } from "hull:web:pagination";
import { csp }        from "hull:web:middleware:csp";
import { csrf }       from "hull:web:middleware:csrf";
import { session }    from "hull:web:middleware:session";
import { cookie }     from "hull:web:cookie";
import { template }   from "hull:template";
import { form }       from "hull:web:form";
import { log }        from "hull:log";
import { db }         from "hull:db";

// Small default so pagination is visible in the demo with only a
// handful of todos. Real apps would set this to 20-50.
const PER_PAGE_DEFAULT = 3;

app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/web/htmx@1",
        "hull/web/flash@1",
        "hull/web/pagination@1",
        "hull/web/middleware/csp@1",
        "hull/web/middleware/csrf@1",
        "hull/web/middleware/session@1",
        "hull/web/cookie@1",
        "hull/template@1",
        "hull/web/form@1",
        "hull/db@1",
        "hull/log@1",
    ],
});

session.init({ ttl: 86400 });

// CSRF secret. CHANGE-ME-IN-PRODUCTION is the placeholder shipped by
// the scaffold; load from env (or a sealed secret) before deploying.
// The scaffold logs a one-time warning at startup if it's still the
// placeholder so a deploy can't silently inherit it.
const CSRF_SECRET = "CHANGE-ME-IN-PRODUCTION";
if (CSRF_SECRET === "CHANGE-ME-IN-PRODUCTION") {
    log.warn(
        "WARNING: CSRF secret is the scaffold placeholder. " +
        "Replace `CSRF_SECRET` in app.js with a real high-entropy " +
        "value before deploying (load from env, etc.)."
    );
}

// Bootstrap anonymous session per request. `secure: false` is
// intentional for the scaffold: it lets `hull dev` (plain HTTP on
// :8080) work in a real browser. Production over HTTPS should set
// `secure: true` so the cookie is only sent over TLS.
//
// CSRF reads the session id from req.ctx.session_id (we set it
// below), so no req.headers patching is needed on this same request.
function sessionBootstrap(req, res) {
    req.ctx = req.ctx || {};
    const cookies = cookie.parse(req.headers["cookie"] || "");
    let sid = cookies.hull_session;
    if (sid && session.load(sid)) {
        req.ctx.session_id = sid;
        return 0;
    }
    sid = session.create({});
    req.ctx.session_id = sid;
    res.header("Set-Cookie", cookie.serialize("hull_session", sid, { secure: false }));
    return 0;
}

app.use("*", "/*", csp.htmx());
app.use("*", "/*", sessionBootstrap);
// cookieName matches sessionBootstrap's cookie name (the default
// `hull.sid` would not). Falls back to req.ctx.session_id first
// thanks to the v0.1.8 sessionKey addition, so this is belt-and-
// suspenders for cases where the cookie is the only available source
// (e.g., a request with no upstream session middleware).
app.usePost("*", "/*", csrf.middleware({
    secret: CSRF_SECRET,
    cookieName: "hull_session",
}));

app.get("/", (req, res) => {
    // flash.consume drains any pending one-shot messages from the
    // previous POST/redirect/GET cycle and clears them from session.
    //
    // Template literal keys are snake_case to match the Lua sibling +
    // the actual ctx keys (csp.js writes csp_nonce; csrf.js writes
    // csrf_token). Same template HTML works for both runtimes.
    const msgs = flash.consume(req);

    // pagination.fromQuery reads ?page=N&per_page=M; builds
    // SQL-ready offset+limit. render produces a nav structure
    // with windowed links + ellipses for large page counts.
    const p = pagination.fromQuery(req, {
        defaultPerPage: PER_PAGE_DEFAULT,
    });
    const total = db.query("SELECT COUNT(*) AS n FROM todos")[0].n;
    const todos = db.query(
        "SELECT id, title, done FROM todos ORDER BY id DESC "
        + "LIMIT ? OFFSET ?",
        [p.limit, p.offset]
    );
    const nav = pagination.render(total, {
        page:           p.page,
        per_page:       p.per_page,
        defaultPerPage: PER_PAGE_DEFAULT,
        baseUrl:        "/",
    });

    res.html(template.render("pages/home.html", {
        csp_nonce:  req.ctx.csp_nonce,
        csrf_token: req.ctx.csrf_token,
        todos:      todos,
        pagination: nav,
        flash:      msgs,
        has_flash:  msgs.length > 0,
    }));
});

app.post("/todos", (req, res) => {
    const fields = form.parse(req.body || "");
    const title = (fields.title || "").trim();
    if (!title) {
        htmx.retarget(res, "#new-todo");
        res.html('<p id="new-todo" role="alert">Title cannot be empty.</p>');
        return;
    }
    db.exec("INSERT INTO todos (title, done) VALUES (?, 0)", [title]);
    const id = db.query("SELECT last_insert_rowid() AS id")[0].id;
    if (htmx.is(req)) {
        // HTMX: row + fresh form fragment.
        // flash.trigger fires a client-side 'flash' event that any
        // listener (e.g. a toast widget) can render. Independent of
        // the OOB swap path; HTMX-only.
        // todo_row.html uses `{{ t.X }}` so the same partial works
        // both here (single render) and inside the GET / for-loop
        // (`{% for t in todos %}{% include %}{% end %}`).
        flash.trigger(res, "Added: " + title, "success");
        res.html(
            template.render("partials/todo_row.html",  { t: { id, title, done: false } })
            + template.render("partials/todo_form.html", { csrf_token: req.ctx.csrf_token })
        );
    } else {
        // Plain form post: stash a message in session, redirect.
        // The next GET / will consume + render it.
        flash.set(req, "Added: " + title, "success");
        res.redirect("/");
    }
});

// Search with debounced HTMX trigger. The input on home.html fires
// `hx-get="/search"` on `keyup changed delay:300ms` so a user can
// type freely; only the final settled value hits the server. The
// response is just the <li> rows; hx-target="#todos" replaces the
// inner HTML of the list. Pagination is intentionally not part of
// the search result — searches always show up to 20 matches.
app.get("/search", (req, res) => {
    const q = (((req.query && req.query.q) || "")).trim();
    let rows;
    if (q === "") {
        rows = db.query(
            "SELECT id, title, done FROM todos "
            + "ORDER BY id DESC LIMIT 20"
        );
    } else {
        rows = db.query(
            "SELECT id, title, done FROM todos "
            + "WHERE title LIKE ? ORDER BY id DESC LIMIT 20",
            ["%" + q + "%"]
        );
    }
    if (htmx.is(req)) {
        if (rows.length === 0) {
            res.html('<li class="muted">No matches.</li>');
        } else {
            const parts = rows.map(row =>
                template.render("partials/todo_row.html", { t: row }));
            res.html(parts.join(""));
        }
    } else {
        res.redirect("/");
    }
});

app.post("/todos/:id/toggle", (req, res) => {
    const id = parseInt(req.params.id, 10);
    db.exec("UPDATE todos SET done = NOT done WHERE id = ?", [id]);
    const row = db.query("SELECT id, title, done FROM todos WHERE id = ?", [id])[0];
    if (!row) { res.status(404); return; }
    if (htmx.is(req)) {
        res.html(template.render("partials/todo_row.html", { t: row }));
    } else {
        res.redirect("/");
    }
});

// Inline edit. Triad of routes:
//   GET   /todos/:id/edit   -> swap row to inline edit form
//   GET   /todos/:id        -> show a single row (used by Cancel)
//   PATCH /todos/:id        -> save edit, return row fragment (or
//                              re-render edit form on validation error)
// Order matters: register the MORE SPECIFIC path (/edit) first.
// Hull's router is first-match, and the bare /todos/:id pattern
// would otherwise greedily capture "123/edit" as the :id.
// Plain-form fallback for PATCH: see docs/htmx.md (Rails-style
// POST + _method=PATCH override). The example is HTMX-only.

app.get("/todos/:id/edit", (req, res) => {
    const id = parseInt(req.params.id, 10);
    const row = db.query(
        "SELECT id, title, done FROM todos WHERE id = ?", [id])[0];
    if (!row) { res.status(404); return; }
    res.html(template.render("partials/_todo_edit_form.html", {
        t: row,
        csrf_token: req.ctx.csrf_token,
    }));
});

app.get("/todos/:id", (req, res) => {
    const id = parseInt(req.params.id, 10);
    const row = db.query(
        "SELECT id, title, done FROM todos WHERE id = ?", [id])[0];
    if (!row) { res.status(404); return; }
    if (htmx.is(req)) {
        res.html(template.render("partials/todo_row.html", { t: row }));
    } else {
        res.redirect("/");
    }
});

app.patch("/todos/:id", (req, res) => {
    const id = parseInt(req.params.id, 10);
    const fields = form.parse(req.body || "");
    const title = (fields.title || "").trim();
    if (title === "") {
        // Re-render the edit form with an inline error.
        const existing = db.query(
            "SELECT id, title, done FROM todos WHERE id = ?", [id])[0];
        if (!existing) { res.status(404); return; }
        existing.title = "";
        res.html(template.render("partials/_todo_edit_form.html", {
            t: existing,
            csrf_token: req.ctx.csrf_token,
            error: "Title cannot be empty.",
        }));
        return;
    }
    db.exec("UPDATE todos SET title = ? WHERE id = ?", [title, id]);
    const row = db.query(
        "SELECT id, title, done FROM todos WHERE id = ?", [id])[0];
    if (!row) { res.status(404); return; }
    res.html(template.render("partials/todo_row.html", { t: row }));
});

app.delete("/todos/:id", (req, res) => {
    const id = parseInt(req.params.id, 10);
    db.exec("DELETE FROM todos WHERE id = ?", [id]);
    if (htmx.is(req)) {
        res.html("");
    } else {
        res.redirect("/");
    }
});

log.info("hypermedia app loaded");
