// HTMX + Pico hypermedia app scaffold.
// Returns full pages for plain navigation; returns fragments when
// HX-Request is set. CSRF + per-request CSP nonce wired in by default.
import { app }      from "hull:app";
import { htmx }     from "hull:htmx";
import { csp }      from "hull:middleware:csp";
import { csrf }     from "hull:middleware:csrf";
import { session }  from "hull:middleware:session";
import { cookie }   from "hull:cookie";
import { template } from "hull:template";
import { form }     from "hull:form";
import { log }      from "hull:log";
import { db }       from "hull:db";

app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/htmx@1",
        "hull/middleware/csp@1",
        "hull/middleware/csrf@1",
        "hull/middleware/session@1",
        "hull/cookie@1",
        "hull/template@1",
        "hull/form@1",
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

function todosData() {
    return db.query("SELECT id, title, done FROM todos ORDER BY id DESC");
}

app.get("/", (req, res) => {
    // Template literal keys are snake_case to match the Lua sibling +
    // the actual ctx keys (csp.js writes csp_nonce; csrf.js writes
    // csrf_token). Same template HTML works for both runtimes.
    res.html(template.render("pages/home.html", {
        csp_nonce:  req.ctx.csp_nonce,
        csrf_token: req.ctx.csrf_token,
        todos:      todosData(),
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
        res.html(
            template.render("partials/todo_row.html",  { id, title, done: false })
            + template.render("partials/todo_form.html", { csrf_token: req.ctx.csrf_token })
        );
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
        res.html(template.render("partials/todo_row.html", row));
    } else {
        res.redirect("/");
    }
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
