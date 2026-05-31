// Tests for both plain HTML and HTMX request paths.
// All requests pass `middleware: true` so the full middleware chain
// (CSP nonce, session bootstrap, CSRF) runs; without it test.get/post
// dispatches the handler directly and skips middleware.

test("GET / returns full HTML page", async () => {
    const res = await test.get("/", { middleware: true });
    test.eq(res.status, 200);
    test.ok(res.body.includes("<!doctype html>"), "should be a full page");
    test.ok(res.body.includes('id="todos"'), "should contain todo list");
});

test("POST /todos with hx-request returns fragment, not redirect", async () => {
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    test.ok(token && token.length > 0, "csrf token should be in form");
    const res = await test.post("/todos", {
        middleware: true,
        body: "title=buy+milk&_csrf=" + token,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": home.headers["set-cookie"] || "",
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    test.ok(res.body.includes("buy milk"), "fragment should contain new todo");
    test.ok(!res.body.includes("<!doctype"), "fragment must NOT be full page");
});

test("HTMX POST /todos fires flash trigger via HX-Trigger header", async () => {
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const res = await test.post("/todos", {
        middleware: true,
        body: "title=eggs&_csrf=" + token,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": home.headers["set-cookie"] || "",
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    const trig = res.headers["hx-trigger"];
    test.ok(trig && trig.includes('"flash"'),
            "HX-Trigger should carry flash event, got: " + trig);
    test.ok(trig.includes("Added: eggs"),
            "flash payload should include the message");
});

test("plain POST /todos sets session flash; next GET renders it", async () => {
    // POST without hx-request → redirect path → flash.set
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const cookieHdr = home.headers["set-cookie"] || "";
    const post = await test.post("/todos", {
        middleware: true,
        body: "title=plain+post+todo&_csrf=" + token,
        headers: {
            "content-type": "application/x-www-form-urlencoded",
            "cookie": cookieHdr,
            "x-csrf-token": token,
        },
    });
    test.ok(post.status === 302 || post.status === 303,
            "plain POST should redirect, got: " + post.status);
    // Next GET on same session should render the flash.
    const nextGet = await test.get("/", {
        middleware: true,
        headers: { "cookie": cookieHdr },
    });
    test.eq(nextGet.status, 200);
    test.ok(nextGet.body.includes("Added: plain post todo"),
            "next render should include flash message");
    // And the message is one-shot — a SECOND GET should not contain it.
    const thirdGet = await test.get("/", {
        middleware: true,
        headers: { "cookie": cookieHdr },
    });
    test.ok(!thirdGet.body.includes("Added: plain post todo"),
            "flash is one-shot; second render must not include it");
});

test("GET / paginates with ?page=N (default per_page=3 in this demo)", async () => {
    // Distinctive titles so substring matches don't false-positive.
    const { db } = await import("hull:db");
    db.exec("DELETE FROM todos");
    for (let i = 1; i <= 7; i++) {
        db.exec("INSERT INTO todos (title, done) VALUES (?, 0)",
                ["pgN-" + i + "-end"]);
    }
    const cnt = db.query("SELECT COUNT(*) AS n FROM todos")[0].n;
    test.eq(cnt, 7, "expected 7 todos after seed");

    const page1 = await test.get("/", { middleware: true });
    test.eq(page1.status, 200);
    test.ok(page1.body.includes('class="pagination"'),
            "page 1 should render pagination nav");
    test.ok(page1.body.includes("pgN-7-end"),
            "page 1 should contain newest todo");
    test.ok(!page1.body.includes("pgN-1-end"),
            "page 1 should NOT contain oldest todo");

    const page2 = await test.get("/?page=2", { middleware: true });
    test.eq(page2.status, 200);
    test.ok(page2.body.includes("pgN-3-end"),
            "page 2 should contain mid-range todo");
    test.ok(!page2.body.includes("pgN-7-end"),
            "page 2 should NOT contain page-1 todo");

    const page3 = await test.get("/?page=3", { middleware: true });
    test.eq(page3.status, 200);
    test.ok(page3.body.includes("pgN-1-end"),
            "page 3 should contain oldest todo");
});

test("POST /todos with empty title returns validation fragment", async () => {
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const res = await test.post("/todos", {
        middleware: true,
        body: "title=&_csrf=" + token,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": home.headers["set-cookie"] || "",
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    test.ok(res.body.includes("cannot be empty"), "should show validation error");
    test.eq(res.headers["hx-retarget"], "#new-todo",
            "should retarget to the form, not the list");
});
