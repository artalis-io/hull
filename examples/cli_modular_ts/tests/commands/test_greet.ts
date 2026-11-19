// test.runMain(args, opts) runs app.main once with ctx.args = args and
// returns { code, stdout, stderr } (ctx.stdout / ctx.stderr captured).
// opts: stdin (string or bytes), env (names from manifest.env), timeoutMs.

test("greet: prints a greeting", () => {
    const r = test.runMain(["greet", "world"]);
    test.eq(r.code, 0);
    test.eq(r.stdout, "hello world\n");
    test.eq(r.stderr, "");
});

test("greet: a missing name is a usage error", () => {
    const r = test.runMain(["greet"]);
    test.eq(r.code, 2);
    test.ok(r.stderr.includes("missing NAME"), r.stderr);
});

test("an unknown command prints usage", () => {
    const r = test.runMain(["nope"]);
    test.eq(r.code, 1);
    test.ok(r.stderr.includes("unknown command"), r.stderr);
});
