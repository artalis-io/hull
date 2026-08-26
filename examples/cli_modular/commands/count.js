// commands/count.js - Count items passed as arguments.

export function run(ctx) {
    ctx.stdout.write(`${String(ctx.args.length)}\n`);
    return 0;
}
