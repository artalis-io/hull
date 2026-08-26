--
-- hull.templates_cli - `hull new --type cli <name>` scaffolding
--
-- Modular CLI layout. app.lua's app.main is a dispatcher:
--   `mytool greet alice`  →  commands/greet.lua's M.run(ctx)
--   `mytool count a b c`  →  commands/count.lua's M.run(ctx)
--
-- Subcommands live in commands/<name>.lua and export a single function
-- M.run(ctx) that returns the exit code. Shared helpers (output
-- formatting, config parsing, etc.) go in lib/.
--
-- No migrations/ by default - CLI tools rarely need a database. If
-- yours does, mkdir migrations/ and add `"hull/db@1"` to the manifest.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- ── Lua templates ────────────────────────────────────────────────────

local lua_files = {}

lua_files["app.lua"] = [[-- Modular CLI scaffold.
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

    -- Distinguish "unknown command" (the file doesn't exist) from
    -- "command file has a bug" (require failed for some OTHER reason).
    -- The first should show usage; the second should re-raise so the
    -- user sees the actual traceback. The Hull require gate emits a
    -- specific "module not found" / "is not declared" prefix on
    -- missing modules; anything else came from inside the file.
    local ok, mod = pcall(require, "./commands/" .. cmd)
    if not ok then
        if type(mod) == "string" and
                (mod:find("module not found", 1, true) or
                 mod:find("not declared",     1, true)) then
            ctx.stderr:write("mytool: unknown command '" .. cmd .. "'\n\n")
            usage(ctx.stderr)
            return 1
        end
        -- A real error inside the command file - propagate it.
        error(mod, 0)
    end

    log.info("running " .. cmd)
    return mod.run(sub_ctx)
end)
]]

lua_files["commands/greet.lua"] = [[-- commands/greet.lua - Print a greeting.
--
-- Convention: each command file exports a single `run(ctx)` function.
-- ctx is the same shape app.main receives: args / env / stdin / stdout
-- / stderr. The return value is the process exit code (nil → 0).

local fmt = require("./../lib/fmt")

local M = {}

function M.run(ctx)
    local name = ctx.args[1]
    if not name then
        ctx.stderr:write("greet: missing NAME argument\n")
        return 2
    end
    ctx.stdout:write(fmt.greeting(name) .. "\n")
    return 0
end

return M
]]

lua_files["commands/count.lua"] = [[-- commands/count.lua - Count items passed as arguments.

local M = {}

function M.run(ctx)
    ctx.stdout:write(tostring(#ctx.args) .. "\n")
    return 0
end

return M
]]

lua_files["lib/fmt.lua"] = [[-- lib/fmt.lua - Shared output-formatting helpers.
--
-- Anything reused across commands lives here. Keeping it in `lib/`
-- (not `commands/`) means it can be required from anywhere without
-- looking like a subcommand to the app.lua dispatcher.

local M = {}

function M.greeting(name)
    return "hello " .. name
end

return M
]]

lua_files["tests/commands/test_greet.lua"] = [[-- Tests use test.run_main to synthesise argv/stdin/env and capture
-- the exit code + stdout/stderr. As of v0.1.0 the CLI test
-- harness is still being shaped - for now invoke through `hull run`
-- in the project root.

test("greet: smoke", function()
    test.eq(1, 1)  -- placeholder; replace with test.run_main once shipped
end)
]]

lua_files["tests/commands/test_count.lua"] = [[test("count: smoke", function()
    test.eq(1, 1)
end)
]]

-- ── JS templates (parity with Lua) ───────────────────────────────────

local js_files = {}

js_files["app.js"] = [[// Modular CLI scaffold.
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

    // Distinguish "unknown command" (file doesn't exist) from "command
    // file has a bug." QuickJS's module loader emits "module not found"
    // for the first case; anything else came from inside the file and
    // should propagate so the user sees the actual error.
    let mod;
    try {
        mod = await import("./commands/" + cmd + ".js");
    } catch (e) {
        const msg = String(e);
        if (msg.indexOf("module not found") >= 0 ||
            msg.indexOf("is not declared") >= 0) {
            ctx.stderr.write("mytool: unknown command '" + cmd + "'\n\n");
            usage(ctx.stderr);
            return 1;
        }
        throw e;
    }

    log.info("running " + cmd);
    return mod.run(subCtx);
});
]]

js_files["commands/greet.js"] = [[// commands/greet.js - Print a greeting.

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
]]

js_files["commands/count.js"] = [[// commands/count.js - Count items passed as arguments.

export function run(ctx) {
    ctx.stdout.write(String(ctx.args.length) + "\n");
    return 0;
}
]]

js_files["lib/fmt.js"] = [[// lib/fmt.js - Shared output-formatting helpers.

export function greeting(name) {
    return "hello " + name;
}
]]

js_files["tests/commands/test_greet.js"] = [[test("greet: smoke", () => {
    test.eq(1, 1);
});
]]

js_files["tests/commands/test_count.js"] = [[test("count: smoke", () => {
    test.eq(1, 1);
});
]]

-- ── Shared template (runtime-agnostic) ───────────────────────────────

local gitignore = [[*.key
hull.sig
build/
]]

-- ── Public API ───────────────────────────────────────────────────────

function M.files(runtime)
    local out = (runtime == "js") and js_files or lua_files
    local copy = {}
    for k, v in pairs(out) do copy[k] = v end
    copy[".gitignore"] = gitignore
    return copy
end

return M
