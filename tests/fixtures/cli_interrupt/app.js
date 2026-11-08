import { app } from "hull:app";
// app.main observes SIGINT / SIGTERM (shared/main_signal.h, platform gap 7).
// Modes (ctx.args[0]): handler | none | stuck - see app.lua.
app.manifest({ name: "cli-interrupt" });
app.main(async (ctx) => {
    const mode = ctx.args[0] ?? "handler";
    if (mode === "none") {
        ctx.stdout.write("interrupt: ready\n");
        ctx.stdout.flush();
        ctx.stdout.write("interrupt: buffered\n");   // flushed only by a clean exit
        await hull.sleep(60000);
        return 0;
    }
    if (Object.isExtensible(ctx.signal)) throw new Error("ctx.signal is extensible");
    ctx.signal.onabort = (reason) => {
        ctx.stdout.write(`interrupt: handler ${reason}\n`);
        ctx.stdout.flush();
    };
    ctx.stdout.write("interrupt: ready\n");
    ctx.stdout.flush();
    if (mode === "stuck") {
        await hull.sleep(60000);
        return 0;
    }
    const reason = await Promise.race([hull.sleep(60000), ctx.signal.promise]);
    ctx.stdout.write(`interrupt: main saw ${reason} aborted=${ctx.signal.aborted} reason=${ctx.signal.reason}\n`);
    return 3;
});
