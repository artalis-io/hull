-- test.run_main(args, opts) runs app.main once with ctx.args = args and
-- returns { code, stdout, stderr } (ctx.stdout / ctx.stderr captured).
-- opts: stdin (string), env (names from manifest.env), timeout_ms.

test("greet: prints a greeting", function()
    local r = test.run_main({ "greet", "world" })
    test.eq(r.code, 0)
    test.eq(r.stdout, "hello world\n")
    test.eq(r.stderr, "")
end)

test("greet: a missing name is a usage error", function()
    local r = test.run_main({ "greet" })
    test.eq(r.code, 2)
    test.ok(r.stderr:find("missing NAME", 1, true), r.stderr)
end)

test("an unknown command prints usage", function()
    local r = test.run_main({ "nope" })
    test.eq(r.code, 1)
    test.ok(r.stderr:find("unknown command", 1, true), r.stderr)
end)
