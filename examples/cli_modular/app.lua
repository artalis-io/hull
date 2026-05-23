-- Modular CLI scaffold.
--
-- app.main is a dispatcher: the first argv element is the subcommand
-- name; the rest are forwarded as ctx.args inside the command. Each
-- command lives in commands/<name>.lua and exports M.run(ctx).

app.manifest({
    modules = {
        "hull/log@1",
    },
    -- env = { "HOME", "PATH" },   -- list env vars the commands may read
})

local log = require("hull.log")

local function usage(stderr)
    stderr:write("usage: mytool <command> [args...]\n")
    stderr:write("\n")
    stderr:write("commands:\n")
    stderr:write("  greet NAME      print a greeting\n")
    stderr:write("  count ITEMS...  print the number of items\n")
end

app.main(function(ctx)
    local cmd = ctx.args[1]
    if not cmd or cmd == "-h" or cmd == "--help" then
        usage(ctx.stderr)
        return cmd and 0 or 1
    end

    -- Forward args[2..N] as the command's own args, shifted down.
    local sub_args = {}
    for i = 2, #ctx.args do sub_args[#sub_args + 1] = ctx.args[i] end
    local sub_ctx = {
        args = sub_args, env = ctx.env,
        stdin = ctx.stdin, stdout = ctx.stdout, stderr = ctx.stderr,
    }

    -- pcall the require so a typo in cmd name shows usage rather than
    -- a Lua "module not found" backtrace.
    local ok, mod = pcall(require, "./commands/" .. cmd)
    if not ok then
        ctx.stderr:write("mytool: unknown command '" .. cmd .. "'\n\n")
        usage(ctx.stderr)
        return 1
    end

    log.info("running " .. cmd)
    return mod.run(sub_ctx)
end)
