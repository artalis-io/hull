import { app } from "hull:app";
app.manifest({ modules: [] });
app.main(async (ctx) => {
    // Teardown must cancel the losing/unawaited timer without retaining JS refs.
    hull.sleep(60000);
    await hull.sleep(1);
    if (ctx.args[0] === "reject") throw new Error("expected CLI rejection");
    ctx.stdout.write("poll-cli: settled with a pending timer\n");
    return 0;
});
