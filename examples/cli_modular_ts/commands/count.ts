// commands/count.ts - Count items passed as arguments.

import type { CommandContext } from "./../lib/context.ts";

export function run(ctx: CommandContext): number {
    ctx.stdout.write(`${String(ctx.args.length)}\n`);
    return 0;
}
