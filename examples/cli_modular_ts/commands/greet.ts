// commands/greet.ts - Print a greeting.

import { greeting } from "./../lib/fmt.ts";
import type { CommandContext } from "./../lib/context.ts";

export function run(ctx: CommandContext): number {
    const name = ctx.args[0];
    if (!name) {
        ctx.stderr.write("greet: missing NAME argument\n");
        return 2;
    }
    ctx.stdout.write(`${greeting(name)}\n`);
    return 0;
}
