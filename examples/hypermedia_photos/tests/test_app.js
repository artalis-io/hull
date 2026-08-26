// Tests for both plain HTML and HTMX request paths.
// All requests pass `middleware: true` so the full middleware chain
// (CSP nonce, session bootstrap, CSRF) runs; without it test.get/post
// dispatches the handler directly and skips middleware.

test("GET / returns full HTML page", async () => {
    const res = await test.get("/", { middleware: true });
    test.eq(res.status, 200);
    test.ok(res.body.includes("<!doctype html>"), "should be a full page");
    test.ok(res.body.includes('id="entries"'), "should contain entry list");
    test.ok(res.body.includes('hx-indicator="#spinner"'),
            "body should declare hx-indicator");
    test.ok(res.body.includes('id="spinner"'),
            "spinner div should be in the page");
});

test("POST /entries with hx-request returns fragment, not redirect", async () => {
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    test.ok(token && token.length > 0, "csrf token should be in form");
    const res = await test.post("/entries", {
        middleware: true,
        body: `title=buy+milk&_csrf=${token}`,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": home.headers["set-cookie"] || "",
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    test.ok(res.body.includes("buy milk"), "fragment should contain new entry");
    test.ok(!res.body.includes("<!doctype"), "fragment must NOT be full page");
});

test("HTMX POST /entries fires flash trigger via HX-Trigger header", async () => {
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const res = await test.post("/entries", {
        middleware: true,
        body: `title=eggs&_csrf=${token}`,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": home.headers["set-cookie"] || "",
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    const trig = res.headers["hx-trigger"];
    test.ok(trig?.includes('"flash"'),
            `HX-Trigger should carry flash event, got: ${trig}`);
    test.ok(trig.includes("Added: eggs"),
            "flash payload should include the message");
});

test("plain POST /entries sets session flash; next GET renders it", async () => {
    // POST without hx-request → redirect path → flash.set
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const cookieHdr = home.headers["set-cookie"] || "";
    const post = await test.post("/entries", {
        middleware: true,
        body: `title=plain+post+entry&_csrf=${token}`,
        headers: {
            "content-type": "application/x-www-form-urlencoded",
            "cookie": cookieHdr,
            "x-csrf-token": token,
        },
    });
    test.ok(post.status === 302 || post.status === 303,
            `plain POST should redirect, got: ${post.status}`);
    // Next GET on same session should render the flash.
    const nextGet = await test.get("/", {
        middleware: true,
        headers: { "cookie": cookieHdr },
    });
    test.eq(nextGet.status, 200);
    test.ok(nextGet.body.includes("Added: plain post entry"),
            "next render should include flash message");
    // And the message is one-shot - a SECOND GET should not contain it.
    const thirdGet = await test.get("/", {
        middleware: true,
        headers: { "cookie": cookieHdr },
    });
    test.ok(!thirdGet.body.includes("Added: plain post entry"),
            "flash is one-shot; second render must not include it");
});

test("GET / paginates with ?page=N (default per_page=3 in this demo)", async () => {
    // Distinctive titles so substring matches don't false-positive.
    const { db } = await import("hull:db");
    db.exec("DELETE FROM entries");
    for (let i = 1; i <= 7; i++) {
        db.exec("INSERT INTO entries (title, done) VALUES (?, 0)",
                [`pgN-${i}-end`]);
    }
    const cnt = db.query("SELECT COUNT(*) AS n FROM entries")[0].n;
    test.eq(cnt, 7, "expected 7 entries after seed");

    const page1 = await test.get("/", { middleware: true });
    test.eq(page1.status, 200);
    test.ok(page1.body.includes('class="pagination"'),
            "page 1 should render pagination nav");
    test.ok(page1.body.includes("pgN-7-end"),
            "page 1 should contain newest entry");
    test.ok(!page1.body.includes("pgN-1-end"),
            "page 1 should NOT contain oldest entry");

    const page2 = await test.get("/?page=2", { middleware: true });
    test.eq(page2.status, 200);
    test.ok(page2.body.includes("pgN-3-end"),
            "page 2 should contain mid-range entry");
    test.ok(!page2.body.includes("pgN-7-end"),
            "page 2 should NOT contain page-1 entry");

    const page3 = await test.get("/?page=3", { middleware: true });
    test.eq(page3.status, 200);
    test.ok(page3.body.includes("pgN-1-end"),
            "page 3 should contain oldest entry");
});

test("GET /search filters by title (HTMX fragment)", async () => {
    const { db } = await import("hull:db");
    db.exec("DELETE FROM entries");
    db.exec("INSERT INTO entries (title, done) VALUES ('buy milk', 0)");
    db.exec("INSERT INTO entries (title, done) VALUES ('buy eggs', 0)");
    db.exec("INSERT INTO entries (title, done) VALUES ('write report', 0)");

    const hit = await test.get("/search?q=milk",
        { middleware: true, headers: { "hx-request": "true" } });
    test.eq(hit.status, 200);
    test.ok(hit.body.includes("buy milk"),
            "matching entry should be in result");
    test.ok(!hit.body.includes("report"),
            "non-matching entry should NOT be in result");

    const miss = await test.get("/search?q=zzzzz",
        { middleware: true, headers: { "hx-request": "true" } });
    test.eq(miss.status, 200);
    test.ok(miss.body.includes("No entries match"),
            "empty result should render the empty-state row");

    const all = await test.get("/search?q=",
        { middleware: true, headers: { "hx-request": "true" } });
    test.eq(all.status, 200);
    test.ok(all.body.includes("buy milk"));
    test.ok(all.body.includes("buy eggs"));
    test.ok(all.body.includes("write report"));
});

test("inline edit: GET /entries/:id/edit returns form, PATCH saves + returns row", async () => {
    const { db } = await import("hull:db");
    db.exec("DELETE FROM entries");
    db.exec("INSERT INTO entries (title, done) VALUES ('original title', 0)");
    const id = db.query("SELECT id FROM entries LIMIT 1")[0].id;

    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const cookieHdr = home.headers["set-cookie"] || "";
    const hxHeaders = {
        "hx-request": "true",
        "cookie": cookieHdr,
        "x-csrf-token": token,
    };

    const edit = await test.get(`/entries/${id}/edit`,
        { middleware: true, headers: hxHeaders });
    test.eq(edit.status, 200);
    test.ok(edit.body.includes(`hx-patch="/entries/${id}"`),
            "edit form should use hx-patch");
    test.ok(edit.body.includes('value="original title"'),
            "edit form should pre-fill current title");

    const saved = await test.patch(`/entries/${id}`, {
        middleware: true,
        body: `title=updated+title&_csrf=${token}`,
        headers: {
            ...hxHeaders,
            "content-type": "application/x-www-form-urlencoded",
        },
    });
    test.eq(saved.status, 200);
    test.ok(saved.body.includes("updated title"),
            "PATCH response should contain new title");
    test.ok(!saved.body.includes('hx-patch="'),
            "PATCH success should return the row, NOT the edit form");

    const stored = db.query(
        "SELECT title FROM entries WHERE id = ?", [id])[0].title;
    test.eq(stored, "updated title");

    const cancel = await test.get(`/entries/${id}`,
        { middleware: true, headers: hxHeaders });
    test.eq(cancel.status, 200);
    test.ok(cancel.body.includes("updated title"));
    test.ok(!cancel.body.includes('hx-patch="'));
});

test("inline edit: PATCH with empty title re-renders edit form with error", async () => {
    const { db } = await import("hull:db");
    db.exec("DELETE FROM entries");
    db.exec("INSERT INTO entries (title, done) VALUES ('keep me', 0)");
    const id = db.query("SELECT id FROM entries LIMIT 1")[0].id;

    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const cookieHdr = home.headers["set-cookie"] || "";

    const res = await test.patch(`/entries/${id}`, {
        middleware: true,
        body: `title=&_csrf=${token}`,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": cookieHdr,
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    test.ok(res.body.includes("Title cannot be empty"),
            "validation error should be in the body");
    test.ok(res.body.includes(`hx-patch="/entries/${id}"`),
            "response should re-render the edit form");

    const stored = db.query(
        "SELECT title FROM entries WHERE id = ?", [id])[0].title;
    test.eq(stored, "keep me");
});

test("idempotency: repeating POST with same Idempotency-Key writes once", async () => {
    const { db } = await import("hull:db");
    db.exec("DELETE FROM entries");

    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const cookieHdr = home.headers["set-cookie"] || "";
    const body = `title=idem+probe&_csrf=${token}`;
    const headers = {
        "hx-request": "true",
        "content-type": "application/x-www-form-urlencoded",
        "cookie": cookieHdr,
        "x-csrf-token": token,
        "idempotency-key": "test-key-001",
    };

    const first = await test.post("/entries", {
        middleware: true, body, headers,
    });
    test.eq(first.status, 200);

    const second = await test.post("/entries", {
        middleware: true, body, headers,
    });
    test.eq(second.status, 200);
    test.eq(second.headers["x-idempotency-replay"], "true",
            "second submit should be a replay");

    const cnt = db.query("SELECT COUNT(*) AS n FROM entries")[0].n;
    test.eq(cnt, 1, "should not have double-inserted");
});

test("POST /entries with empty title re-renders form with error", async () => {
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const res = await test.post("/entries", {
        middleware: true,
        body: `title=&_csrf=${token}`,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": home.headers["set-cookie"] || "",
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    test.ok(res.body.includes("cannot be empty"), "should show validation error");
    test.eq(res.headers["hx-retarget"], "#new-entry",
            "should retarget to the form, not the list");
    test.ok(res.body.includes('name="title"'),
            "should re-render the form with the title input");
    test.ok(res.body.includes('aria-invalid="true"'),
            "field should be marked aria-invalid");
});

test("POST /entries with too-long title preserves submitted value in re-render", async () => {
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const longTitle = "a".repeat(201);
    const res = await test.post("/entries", {
        middleware: true,
        body: `title=${longTitle}&_csrf=${token}`,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": home.headers["set-cookie"] || "",
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    test.ok(res.body.includes(`value="${longTitle}"`),
            "submitted (too-long) value should be pre-filled in the input");
    test.ok(res.body.includes('<small role="alert"'),
            "should show a per-field error message");
});
