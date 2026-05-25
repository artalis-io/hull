// Hello CLI — Hull + JS app.main example
//
// Run: hull run app.js -- world
//      hull run app.js -- alice
//      echo "stuff" | hull run app.js --stdin
//
// Demonstrates the CLI-mode (app.main) entry point: argv via ctx.args,
// env vars via ctx.env, stdin/stdout/stderr via ctx streams, exit code
// via the return value. Mutually exclusive with app.get/post/etc.

import { app } from "hull:app";
import { crypto } from "hull:crypto";

app.manifest({
    modules: [
    "hull/http-server@1",
        "hull/crypto@1",
    ],
    env: ["USER", "LANG"],
});

function printUsage(stderr) {
    stderr.write("usage: hull run app.js -- <name>\n");
    stderr.write("       hull run app.js --stdin   (read greeting target from stdin)\n");
}

app.main((ctx) => {
    let readStdin = false;
    for (const a of ctx.args) {
        if (a === "--stdin") readStdin = true;
        if (a === "-h" || a === "--help") {
            printUsage(ctx.stderr);
            return 0;
        }
    }

    let name;
    if (readStdin) {
        name = ctx.stdin.read("l");
        if (!name || name === "") {
            ctx.stderr.write("error: no input on stdin\n");
            return 2;
        }
    } else {
        name = ctx.args[0];
        if (!name) {
            printUsage(ctx.stderr);
            return 1;
        }
    }

    const greeting = `hello ${name}`;
    const digest = crypto.sha256(greeting);
    const user = ctx.env.USER || "unknown";

    ctx.stdout.write(`${greeting}\n`);
    ctx.stdout.write(`  digest = ${digest.slice(0, 16)}...\n`);
    ctx.stdout.write(`  user   = ${user}\n`);
    return 0;
});
