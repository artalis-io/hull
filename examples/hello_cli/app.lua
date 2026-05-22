-- Hello CLI — Hull + Lua app.main example
--
-- Run: hull run app.lua -- world
--      hull run app.lua -- alice
--      echo "stuff" | hull run app.lua --stdin
--
-- Demonstrates the CLI-mode (app.main) entry point: argv via ctx.args,
-- env vars via ctx.env, stdin/stdout/stderr via ctx streams, exit code
-- via the return value. Mutually exclusive with app.get/post/etc.

local crypto = require("hull.crypto")

app.manifest({
    modules = {
    "hull/http-server@1",
        "hull/crypto@1",
    },
    env = { "USER", "LANG" },
})

local function print_usage(stderr)
    stderr:write("usage: hull run app.lua -- <name>\n")
    stderr:write("       hull run app.lua --stdin   (read greeting target from stdin)\n")
end

app.main(function(ctx)
    -- --stdin flag → read one line from stdin
    local read_stdin = false
    for _, a in ipairs(ctx.args) do
        if a == "--stdin" then read_stdin = true end
        if a == "-h" or a == "--help" then
            print_usage(ctx.stderr)
            return 0
        end
    end

    local name
    if read_stdin then
        name = ctx.stdin:read("*l")
        if not name or name == "" then
            ctx.stderr:write("error: no input on stdin\n")
            return 2
        end
    else
        name = ctx.args[1]
        if not name then
            print_usage(ctx.stderr)
            return 1
        end
    end

    -- Greet with a deterministic salt so test output is stable.
    local greeting = "hello " .. name
    local digest = crypto.sha256(greeting)
    local user = ctx.env.USER or "unknown"

    ctx.stdout:write(greeting .. "\n")
    ctx.stdout:write("  digest = " .. string.sub(digest, 1, 16) .. "...\n")
    ctx.stdout:write("  user   = " .. user .. "\n")
    return 0
end)
