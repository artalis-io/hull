import { app } from "hull:app";
// app.main reads stdin AFTER `await ctx.signal.promise`: that continuation is
// main's own code, so ctx.stdin.read() must be available there (audit 44).
app.manifest({ name: "cli-interrupt-stdin" });
app.main(async (ctx) => {
    ctx.signal.onabort = (reason) => {
        ctx.stdout.write(`interrupt: handler ${reason}\n`);
        ctx.stdout.flush();
    };
    ctx.stdout.write("interrupt: ready\n");
    ctx.stdout.flush();
    await ctx.signal.promise;
    let msg;
    try { msg = `true ${await ctx.stdin.read("l")}`; } catch (e) { msg = `false ${e.message}`; }
    ctx.stdout.write(`interrupt: stdin ${msg}\n`);
    ctx.stdout.flush();
    return 4;
});
