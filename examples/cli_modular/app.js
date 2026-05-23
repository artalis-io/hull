// Modular CLI scaffold.
//
// app.main is a dispatcher: the first argv element is the subcommand
// name; the rest are forwarded as ctx.args inside the command. Each
// command lives in commands/<name>.js and exports `run(ctx)`.

import { app } from "hull:app";
import { log } from "hull:log";

app.manifest({
    modules: [
        "hull/log@1",
    ],
    // env: ["HOME", "PATH"],
});

function usage(stderr) {
    stderr.write("usage: mytool <command> [args...]\n");
    stderr.write("\n");
    stderr.write("commands:\n");
    stderr.write("  greet NAME      print a greeting\n");
    stderr.write("  count ITEMS...  print the number of items\n");
}

app.main(async (ctx) => {
    const cmd = ctx.args[0];
    if (!cmd || cmd === "-h" || cmd === "--help") {
        usage(ctx.stderr);
        return cmd ? 0 : 1;
    }

    const subCtx = {
        args:   ctx.args.slice(1),
        env:    ctx.env,
        stdin:  ctx.stdin,
        stdout: ctx.stdout,
        stderr: ctx.stderr,
    };

    let mod;
    try {
        mod = await import("./commands/" + cmd + ".js");
    } catch (_e) {
        ctx.stderr.write("mytool: unknown command '" + cmd + "'\n\n");
        usage(ctx.stderr);
        return 1;
    }

    log.info("running " + cmd);
    return mod.run(subCtx);
});
