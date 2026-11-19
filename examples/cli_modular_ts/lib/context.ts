// lib/context.ts - The context app.main hands each subcommand.

export type CommandContext = Pick<Hull.MainContext, "env" | "stdin" | "stdout" | "stderr"> & {
    args: string[];
};
