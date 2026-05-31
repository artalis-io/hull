-- Tests for both plain HTML and HTMX request paths.
-- All requests use opts.middleware = true so the full middleware chain
-- (CSP nonce, session bootstrap, CSRF) runs; without it test.get/post
-- dispatches the handler directly and skips middleware.

test("GET / returns full HTML page", function()
    local res = test.get("/", { middleware = true })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "<!doctype html>"), "should be a full page")
    test.ok(string.find(res.body, "id=\"todos\""), "should contain todo list")
end)

test("POST /todos with hx-request returns fragment, not redirect", function()
    -- Get a CSRF token + session cookie by hitting / first.
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    test.ok(token and #token > 0, "csrf token should be in form")
    -- Send htmx-flavored POST with token + cookie.
    local res = test.post("/todos", {
        middleware = true,
        body = "title=buy+milk&_csrf=" .. token,
        headers = {
            ["hx-request"] = "true",
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = home.headers["set-cookie"] or "",
            ["x-csrf-token"] = token,
        },
    })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "buy milk"), "fragment should contain new todo")
    test.ok(not string.find(res.body, "<!doctype"), "fragment must NOT be full page")
end)

test("HTMX POST /todos fires flash trigger via HX-Trigger header", function()
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local res = test.post("/todos", {
        middleware = true,
        body = "title=eggs&_csrf=" .. token,
        headers = {
            ["hx-request"] = "true",
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = home.headers["set-cookie"] or "",
            ["x-csrf-token"] = token,
        },
    })
    test.eq(res.status, 200)
    local trig = res.headers["hx-trigger"]
    test.ok(trig and trig:find('"flash"', 1, true),
            "HX-Trigger should carry flash event, got: " .. tostring(trig))
    test.ok(trig:find("Added: eggs", 1, true),
            "flash payload should include the message")
end)

test("plain POST /todos sets session flash; next GET renders it", function()
    -- POST without hx-request → redirect path → flash.set
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local cookie_hdr = home.headers["set-cookie"] or ""
    local post = test.post("/todos", {
        middleware = true,
        body = "title=plain+post+todo&_csrf=" .. token,
        headers = {
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = cookie_hdr,
            ["x-csrf-token"] = token,
        },
    })
    -- Redirect path on plain form post.
    test.ok(post.status == 302 or post.status == 303,
            "plain POST should redirect, got: " .. tostring(post.status))
    -- Next GET on same session should render the flash.
    local next_get = test.get("/", {
        middleware = true,
        headers = { ["cookie"] = cookie_hdr },
    })
    test.eq(next_get.status, 200)
    test.ok(string.find(next_get.body, "Added: plain post todo"),
            "next render should include flash message")
    -- And the message is one-shot — a SECOND GET should not contain it.
    local third_get = test.get("/", {
        middleware = true,
        headers = { ["cookie"] = cookie_hdr },
    })
    test.ok(not string.find(third_get.body, "Added: plain post todo"),
            "flash is one-shot; second render must not include it")
end)

test("POST /todos with empty title returns validation fragment", function()
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local res = test.post("/todos", {
        middleware = true,
        body = "title=&_csrf=" .. token,
        headers = {
            ["hx-request"] = "true",
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = home.headers["set-cookie"] or "",
            ["x-csrf-token"] = token,
        },
    })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "cannot be empty"), "should show validation error")
    test.eq(res.headers["hx-retarget"], "#new-todo",
            "should retarget to the form, not the list")
end)
