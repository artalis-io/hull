// commands/greet.js — Print a greeting.

import { greeting } from "./../lib/fmt.js";

export function run(ctx) {
    const name = ctx.args[0];
    if (!name) {
        ctx.stderr.write("greet: missing NAME argument\n");
        return 2;
    }
    ctx.stdout.write(greeting(name) + "\n");
    return 0;
}
