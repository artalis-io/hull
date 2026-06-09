// HTMX + Pico hypermedia app scaffold.
// Returns full pages for plain navigation; returns fragments when
// HX-Request is set. CSRF + per-request CSP nonce wired in by default.
import { app }              from "hull:app";
import { attachment }       from "hull:attachment";
import { blob }             from "hull:blob";
import { db }               from "hull:db";
import { log }              from "hull:log";
import { template }         from "hull:template";
import { time }             from "hull:time";
import { validate }         from "hull:validate";
import { cookie }           from "hull:web:cookie";
import { flash }            from "hull:web:flash";
import { form }             from "hull:web:form";
import { htmx }             from "hull:web:htmx";
import { csp }              from "hull:web:middleware:csp";
import { csrf }             from "hull:web:middleware:csrf";
import { idempotency }      from "hull:web:middleware:idempotency";
import { session }          from "hull:web:middleware:session";
import { pagination }       from "hull:web:pagination";
import { attachmentServe }  from "hull:web:attachment-serve";

// Small default so pagination is visible in the demo with only a
// handful of todos. Real apps would set this to 20-50.
const PER_PAGE_DEFAULT = 3;

app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/web/htmx@1",
        "hull/web/flash@1",
        "hull/web/pagination@1",
        "hull/validate@1",
        "hull/web/middleware/csp@1",
        "hull/web/middleware/csrf@1",
        "hull/web/middleware/session@1",
        "hull/web/middleware/idempotency@1",
        "hull/web/cookie@1",
        "hull/template@1",
        "hull/web/form@1",
        "hull/db@1",
        "hull/log@1",
        // Photo attachments (§1.5.b-5).
        "hull/attachment@1",
        "hull/web/attachment-serve@1",
        "hull/blob@1",
        "hull/fs@1",
        "hull/mime@1",
        "hull/time@1",
    ],
    fs: { write: ["data/"] },
});

session.init({ ttl: 86400 });

// Photo attachments. blob.init + attachment.init have to run after
// the sandbox wires the fs cap, so they go inside app.main (which
// fires once on the event-loop thread, then control falls through
// to the serve loop because we have handlers registered).
app.main(() => {
    blob.init({ dir: "data/blobs" });
    attachment.init({
        maxSize: 4 * 1024 * 1024,
        mimeAllowlist: ["image/png", "image/jpeg", "image/gif", "image/webp"],
    });
});

// Idempotency-key cache (creates _hull_idempotency_keys on first run).
// Only kicks in when the client sends an `Idempotency-Key: <uuid>` header
// - without it, requests pass through normally. HTMX doesn't send the
// header by default; opt in with `hx-headers='{"Idempotency-Key":"..."}'`
// on the form. See docs/htmx.md § Idempotency for the client recipe.
idempotency.init({ ttl: 86400 });

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
// Helper: load attachments for a todo. The _hull_attachments table
// is internal (cap layer blocks direct access), so we go through
// attachment.metadata(id). Used wherever a single row is rendered
// so the photo strip survives toggle / edit / patch.
function attachmentsFor(todoId) {
    const refs = db.query(
        "SELECT attachment_id FROM todo_attachments WHERE todo_id = ? "
        + "ORDER BY created_at DESC",
        [todoId]);
    const out = [];
    for (const r of refs || []) {
        const meta = attachment.metadata(r.attachment_id);
        if (meta) out.push(meta);
    }
    return out;
}

function rowData(row, req) {
    row.attachments = attachmentsFor(row.id);
    return { t: row, csrf_token: req.ctx.csrf_token };
}

function sessionBootstrap(req, res) {
    req.ctx = req.ctx || {};
    const cookies = cookie.parse(req.headers.cookie || "");
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
// Idempotency. Only takes effect when the client sends an
// `Idempotency-Key` header (HTMX double-clicks land here when the
// form opts in via `hx-headers`). Scoped to mutating methods.
app.usePost("POST",  "/*", idempotency.middleware({
    getPrincipal: (req) => req.ctx.session_id != null
                            ? String(req.ctx.session_id) : "__anon",
}));
app.usePost("PATCH", "/*", idempotency.middleware({
    getPrincipal: (req) => req.ctx.session_id != null
                            ? String(req.ctx.session_id) : "__anon",
}));

// Percent-encode for use as a query-string value.
function urlEncode(s) {
    return s.replace(/[^A-Za-z0-9\-._~]/g,
        (c) => "%" + c.charCodeAt(0).toString(16).toUpperCase().padStart(2, "0"));
}

// Build the data needed by partials/_todo_feed.html (todos + pagination)
// plus the page-level extras (csrf_token + csp_nonce). Used by both the
// full-page GET / and the fragment-only GET /search.
function feedData(req, q) {
    const p = pagination.fromQuery(req, { defaultPerPage: PER_PAGE_DEFAULT });
    let total, todos;
    if (q === "") {
        total = db.query("SELECT COUNT(*) AS n FROM todos")[0].n;
        todos = db.query(
            "SELECT id, title, done FROM todos ORDER BY id DESC "
            + "LIMIT ? OFFSET ?",
            [p.limit, p.offset]);
    } else {
        // LIKE-escape % _ \ so "100%" matches the literal "100%"
        // instead of "everything after 100".
        const esc = q.replace(/[\\%_]/g, "\\$&");
        const pattern = "%" + esc + "%";
        total = db.query(
            "SELECT COUNT(*) AS n FROM todos "
            + "WHERE title LIKE ? ESCAPE '\\'", [pattern])[0].n;
        todos = db.query(
            "SELECT id, title, done FROM todos "
            + "WHERE title LIKE ? ESCAPE '\\' "
            + "ORDER BY id DESC LIMIT ? OFFSET ?",
            [pattern, p.limit, p.offset]);
    }
    for (const t of todos) t.attachments = attachmentsFor(t.id);

    // Pagination links must preserve the current query so paging
    // through a filtered list stays filtered.
    let base = "/search";
    if (q !== "") base += "?q=" + urlEncode(q);
    const nav = pagination.render(total, {
        page:           p.page,
        per_page:       p.per_page,
        defaultPerPage: PER_PAGE_DEFAULT,
        baseUrl:        base,
    });

    return {
        csrf_token: req.ctx.csrf_token,
        csp_nonce:  req.ctx.csp_nonce,
        q:          q,
        has_query:  q !== "",
        todos:      todos,
        has_todos:  todos.length > 0,
        pagination: nav,
    };
}

app.get("/", (req, res) => {
    // flash.consume drains any pending one-shot messages from the
    // previous POST/redirect/GET cycle and clears them from session.
    const msgs = flash.consume(req);
    const q = ((req.query && req.query.q) || "").replace(/^\s+|\s+$/g, "");
    const data = feedData(req, q);
    data.flash = msgs;
    data.has_flash = msgs.length > 0;
    res.html(template.render("pages/home.html", data));
});

app.post("/todos", (req, res) => {
    const fields = form.parse(req.body || "");

    // Validate via hull.validate. `trim: true` strips whitespace into
    // the same `fields` object in-place, so a post-validate
    // `fields.title` is already the cleaned value.
    const [ok, errors] = validate.check(fields, {
        title: {
            required: true, trim: true, min: 1, max: 200,
            message: "Title cannot be empty.",
        },
    });
    if (!ok) {
        // Re-render the form fragment with submitted values + per-field
        // error messages. hx-retarget so the response lands on the form
        // itself (the form's own hx-target is #todos, but the validation
        // response should replace #new-todo).
        htmx.retarget(res, "#new-todo");
        htmx.reswap(res, "outerHTML");
        res.html(template.render("partials/todo_form.html", {
            csrf_token: req.ctx.csrf_token,
            values:     fields,
            errors:     errors,
        }));
        return;
    }

    const title = fields.title;  // already trimmed by validate
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
        flash.trigger(res, `Added: ${title}`, "success");
        const html =
            // Return ONLY the new row. The existing #new-todo form
            // resets via hx-on::after-request — re-rendering it here
            // would produce a 2nd form with the same id inside the
            // <ul> via afterbegin (visible bug in earlier versions).
            template.render("partials/todo_row.html",
                { t: { id, title, done: false, attachments: [] },
                  csrf_token: req.ctx.csrf_token });
        // idempotency.respondHtml caches the rendered HTML so a
        // retry with the same Idempotency-Key gets the same response
        // without re-running the handler. No-op when no key is active.
        idempotency.respondHtml(req, res, 200, html);
    } else {
        // Plain form post: stash a message in session, redirect.
        // The next GET / will consume + render it.
        flash.set(req, `Added: ${title}`, "success");
        res.redirect("/");
    }
});

// Search with debounced HTMX trigger. The input on home.html fires
// `hx-get="/search"` on `keyup changed delay:300ms` so a user can
// type freely; only the final settled value hits the server. The
// HTMX path returns just the #todo-feed fragment (rows + pagination
// nav whose links route back through /search?q=... so paging stays
// inside the filtered view). A plain navigation to /search?q=X
// (no HX-Request header) bounces to /?q=X so we re-render the full
// page with the search input pre-filled by the browser's restored
// form state and the URL stays canonical.
app.get("/search", (req, res) => {
    const q = (((req.query?.q) || "")).replace(/^\s+|\s+$/g, "");
    if (htmx.is(req)) {
        const data = feedData(req, q);
        res.html(template.render("partials/_todo_feed.html", data));
    } else {
        res.redirect(q === "" ? "/" : "/?q=" + urlEncode(q));
    }
});

app.post("/todos/:id/toggle", (req, res) => {
    const id = Number.parseInt(req.params.id, 10);
    if (!Number.isInteger(id) || id < 1) { res.status(404); return; }
    db.exec("UPDATE todos SET done = NOT done WHERE id = ?", [id]);
    const row = db.query("SELECT id, title, done FROM todos WHERE id = ?", [id])[0];
    if (!row) { res.status(404); return; }
    if (htmx.is(req)) {
        res.html(template.render("partials/todo_row.html", rowData(row, req)));
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
    const id = Number.parseInt(req.params.id, 10);
    if (!Number.isInteger(id) || id < 1) { res.status(404); return; }
    const row = db.query(
        "SELECT id, title, done FROM todos WHERE id = ?", [id])[0];
    if (!row) { res.status(404); return; }
    res.html(template.render("partials/_todo_edit_form.html", {
        t: row,
        csrf_token: req.ctx.csrf_token,
    }));
});

app.get("/todos/:id", (req, res) => {
    const id = Number.parseInt(req.params.id, 10);
    if (!Number.isInteger(id) || id < 1) { res.status(404); return; }
    const row = db.query(
        "SELECT id, title, done FROM todos WHERE id = ?", [id])[0];
    if (!row) { res.status(404); return; }
    if (htmx.is(req)) {
        res.html(template.render("partials/todo_row.html", rowData(row, req)));
    } else {
        res.redirect("/");
    }
});

app.patch("/todos/:id", (req, res) => {
    const id = Number.parseInt(req.params.id, 10);
    if (!Number.isInteger(id) || id < 1) { res.status(404); return; }
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
    res.html(template.render("partials/todo_row.html", rowData(row, req)));
});

app.delete("/todos/:id", (req, res) => {
    const id = Number.parseInt(req.params.id, 10);
    if (!Number.isInteger(id) || id < 1) { res.status(404); return; }
    // Drop join rows + attached photos before the todo itself.
    // attachment.delete handles refcount, so the on-disk blob unlinks
    // only when no other rows reference it.
    const refs = db.query(
        "SELECT attachment_id FROM todo_attachments WHERE todo_id = ?",
        [id]);
    db.exec("DELETE FROM todo_attachments WHERE todo_id = ?", [id]);
    for (const r of refs || []) attachment["delete"](r.attachment_id);
    db.exec("DELETE FROM todos WHERE id = ?", [id]);
    if (htmx.is(req)) {
        res.html("");
    } else {
        res.redirect("/");
    }
});

// ── Photo attachments (§1.5.b-5) ─────────────────────────────────────
// All routes are scoped under a specific todo. Mirrors the Lua sibling.

// Did the current session post this attachment to this todo? Used as
// the auth gate for both serve + delete.
function ownsAttachment(todoId, attachmentId) {
    const rows = db.query(
        "SELECT 1 FROM todo_attachments WHERE todo_id = ? AND attachment_id = ?",
        [todoId, attachmentId]);
    return rows && rows.length > 0;
}

app.post("/todos/:id/photos", async (req, res) => {
    const todoId = Number.parseInt(req.params.id, 10);
    if (!Number.isInteger(todoId) || todoId < 1) { res.status(400); return; }
    const exists = db.query("SELECT id FROM todos WHERE id = ?", [todoId])[0];
    if (!exists) { res.status(404); return; }

    try {
        for await (const part of req.multipart()) {
            if (part.filename) {
                const attId = await attachment.store(part, {
                    uploadedBy: req.ctx.session_id || "anonymous",
                });
                db.exec(
                    "INSERT INTO todo_attachments (todo_id, attachment_id, created_at) VALUES (?, ?, ?)",
                    [todoId, attId, time.now()]);
            }
        }
    } catch (e) {
        const msg = String((e && e.message) || e);
        if (htmx.is(req)) {
            res.status(413);
            res.html(
                '<small role="alert" class="error">Upload failed: '
                + msg + "</small>");
        } else {
            flash.set(req, "Upload failed: " + msg, "error");
            res.redirect("/");
        }
        return;
    }

    if (htmx.is(req)) {
        res.html(template.render("partials/_attachment_strip.html", {
            t: { id: todoId, attachments: attachmentsFor(todoId) },
        }));
    } else {
        res.redirect("/");
    }
}, { multipart: { maxPartSize: 8 * 1024 * 1024 } });

app.get("/todos/:id/photos/:att_id", (req, res) => {
    const todoId = Number.parseInt(req.params.id, 10);
    const attId = req.params.att_id;
    attachmentServe.serve(req, res, attId, {
        authCheck: (_req, _meta) =>
            Number.isInteger(todoId) && ownsAttachment(todoId, attId),
    });
});

app.delete("/todos/:id/photos/:att_id", (req, res) => {
    const todoId = Number.parseInt(req.params.id, 10);
    const attId = req.params.att_id;
    if (!ownsAttachment(todoId, attId)) { res.status(404); return; }
    db.exec(
        "DELETE FROM todo_attachments WHERE todo_id = ? AND attachment_id = ?",
        [todoId, attId]);
    attachment["delete"](attId);
    if (htmx.is(req)) {
        res.html("");
    } else {
        res.redirect("/");
    }
});

log.info("hypermedia app loaded");
