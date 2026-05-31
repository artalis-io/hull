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
