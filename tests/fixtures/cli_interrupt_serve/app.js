import { app } from "hull:app";
// app.main AND a route (main-then-serve, serve.c runner). main handles the
// interrupt and returns 0 within the grace: the stop request must still end
// the process (audit 44 M6) - it must not fall through into the serve loop.
app.manifest({ name: "cli-interrupt-serve", modules: ["hull/http-server@1"] });
app.get("/ping", (_req, res) => res.text("pong"));
app.main(async (ctx) => {
    ctx.signal.onabort = (reason) => {
        ctx.stdout.write(`interrupt: handler ${reason}\n`);
        ctx.stdout.flush();
    };
    ctx.stdout.write("interrupt: ready\n");
    ctx.stdout.flush();
    const reason = await ctx.signal.promise;
    ctx.stdout.write(`interrupt: main saw ${reason}\n`);
    ctx.stdout.flush();
    return 0;
});
