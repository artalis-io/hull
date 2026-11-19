// Modular CLI - Hull + TypeScript example
//
// The TypeScript twin of examples/cli_modular. app.main is a dispatcher: the
// first argument is the subcommand name, the rest are forwarded as ctx.args
// inside the command. Each command lives in commands/<name>.ts and exports
// `run(ctx)`; a command is imported on demand, by name.
//
// Run:        hull app.ts -- greet world
// Test:       hull test .
// Build:      hull build . && ./app count a b c
// Type-check: hull types . && tsc --noEmit -p .    (tsc or tsgo, from npm)

import { app } from "hull:app";
import { log } from "hull:log";
import type { CommandContext } from "./lib/context.ts";

app.manifest({
    modules: [
        "hull/log@1",
    ],
    // env: ["HOME", "PATH"],
});

function usage(stderr: Hull.MainOutput): void {
    stderr.write("usage: mytool <command> [args...]\n");
    stderr.write("\n");
    stderr.write("commands:\n");
    stderr.write("  greet NAME      print a greeting\n");
    stderr.write("  count ITEMS...  print the number of items\n");
}

app.main(async (ctx: Hull.MainContext): Promise<number> => {
    const cmd = ctx.args[0];
    if (!cmd || cmd === "-h" || cmd === "--help") {
        usage(ctx.stderr);
        return cmd ? 0 : 1;
    }

    const subCtx: CommandContext = {
        args:   ctx.args.slice(1),
        env:    ctx.env,
        stdin:  ctx.stdin,
        stdout: ctx.stdout,
        stderr: ctx.stderr,
    };

    let mod: { run(ctx: CommandContext): number };
    try {
        mod = await import(`./commands/${cmd}.ts`);
    } catch (_e) {
        ctx.stderr.write(`mytool: unknown command '${cmd}'\n\n`);
        usage(ctx.stderr);
        return 1;
    }

    log.info(`running ${cmd}`);
    return mod.run(subCtx);
});
