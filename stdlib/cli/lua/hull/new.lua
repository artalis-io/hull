--
-- hull.new — Scaffold a new Hull project
--
-- Usage: hull new <name> [--runtime lua|js] [--cli]
--
-- Without --cli: scaffolds an HTTP server app (app.get/post/etc).
-- With --cli:    scaffolds a CLI app with an app.main(ctx) entry point.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

-- ── Templates ────────────────────────────────────────────────────────

local templates = {}

templates.lua_app = [[-- Declare every first-party module the app imports.
-- The runtime gate refuses undeclared imports — `hull modules available`
-- lists the full registry. Add capability sections (fs, hosts, env)
-- alongside `modules` when a module needs them.
local log = require("hull.log")
local json = require("hull.json")
local time = require("hull.time")

app.manifest({
    modules = {
        "hull/http-server@1",
        "hull/log@1",
        "hull/time@1",
    },
})

app.get("/", function(_req, res)
    res:json({ status = "ok" })
end)

app.get("/health", function(_req, res)
    res:json({ status = "ok", uptime = time.clock() })
end)

log.info("app loaded")
]]

templates.lua_test = [[test("GET / returns ok", function()
    local res = test.get("/")
    test.eq(res.status, 200)
end)

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.ok(res.json.uptime)
end)
]]

templates.js_app = [[// Declare every first-party module the app imports.
// The runtime gate refuses undeclared imports — `hull modules available`
// lists the full registry. Add capability sections (fs, hosts, env)
// alongside `modules` when a module needs them.
import { app }  from "hull:app";
import { log }  from "hull:log";
import { time } from "hull:time";

app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/log@1",
        "hull/time@1",
    ],
});

app.get("/", (_req, res) => {
    res.json({ status: "ok" });
});

app.get("/health", (_req, res) => {
    res.json({ status: "ok", uptime: time.clock() });
});

log.info("app loaded");
]]

templates.js_test = [[test("GET / returns ok", async () => {
    const res = await test.get("/");
    test.eq(res.status, 200);
});

test("GET /health returns ok", async () => {
    const res = await test.get("/health");
    test.eq(res.status, 200);
    test.ok(res.json.uptime);
});
]]

templates.lua_cli_app = [[-- CLI app — `hull run app.lua [-- args...]` invokes app.main once and exits.
-- `app.main` is mutually exclusive with route registration: pick CLI mode
-- (app.main) or server mode (app.get/etc), not both.

app.manifest({
    -- Declare every first-party module the app imports.
    modules = {},
    -- env = { "HOME", "LANG" },   -- env vars main may read via ctx.env
})

app.main(function(ctx)
    -- ctx.args   : list of argv passed after `--`
    -- ctx.env    : table of env-vars allowed by manifest.env
    -- ctx.stdin  : :read("*l" | "*a" | n), :close()
    -- ctx.stdout : :write(...), :flush()
    -- ctx.stderr : :write(...), :flush()

    local who = ctx.args[1] or "world"
    ctx.stdout:write("hello " .. who .. "\n")
    return 0  -- exit code (nil → 0, integer clamped to 0..255)
end)
]]

templates.lua_cli_test = [[-- CLI tests use test.run_main to synthesize argv/stdin/env and capture
-- the exit code + stdout/stderr. Available once Phase 2 test integration
-- lands; for now invoke main directly via `hull run`.

test("greets the named arg", function()
    -- Phase 2 placeholder — full CLI test harness lands separately.
    test.eq(1, 1)
end)
]]

templates.js_cli_app = [[// CLI app — `hull run app.js [-- args...]` invokes app.main once and exits.
// `app.main` is mutually exclusive with route registration: pick CLI mode
// (app.main) or server mode (app.get/etc), not both.

import { app } from "hull:app";

app.manifest({
    // Declare every first-party module the app imports.
    modules: [],
    // env: ["HOME", "LANG"],   // env vars main may read via ctx.env
});

app.main(async (ctx) => {
    // ctx.args   : Array<string> of argv passed after `--`
    // ctx.env    : Object of env-vars allowed by manifest.env
    // ctx.stdin  : read(...), close()
    // ctx.stdout : write(...), flush()
    // ctx.stderr : write(...), flush()

    const who = ctx.args[0] ?? "world";
    ctx.stdout.write(`hello ${who}\n`);
    return 0;  // exit code (undefined → 0, integer clamped to 0..255)
});
]]

templates.js_cli_test = [[// CLI tests use test.runMain to synthesize argv/stdin/env and capture
// the exit code + stdout/stderr. Available once Phase 2 test integration
// lands; for now invoke main directly via `hull run`.

test("greets the named arg", () => {
    test.eq(1, 1);
});
]]

templates.gitignore = [[data.db
data.db-*
*.key
hull.sig
build/
]]

templates.migration_init = [[-- Migration: 001_init
-- Add your initial schema here
]]

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        name = nil,
        runtime = "lua",
        cli = false,
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--runtime" then
            i = i + 1
            opts.runtime = arg[i]
        elseif a == "--cli" then
            opts.cli = true
        elseif a:sub(1, 1) ~= "-" then
            opts.name = a
        end
        i = i + 1
    end

    return opts
end

-- ── Main ─────────────────────────────────────────────────────────────

local function main()
    local opts = parse_args()

    if not opts.name then
        tool.stderr("Usage: hull new <name> [--runtime lua|js] [--cli]\n")
        tool.exit(1)
    end

    local runtime = opts.runtime
    if runtime ~= "lua" and runtime ~= "js" then
        tool.stderr("hull new: invalid runtime '" .. runtime .. "' (use lua or js)\n")
        tool.exit(1)
    end

    -- Check if directory already exists
    if tool.file_exists(opts.name) then
        tool.stderr("hull new: directory '" .. opts.name .. "' already exists\n")
        tool.exit(1)
    end

    -- Create project structure
    local dir = opts.name
    tool.mkdir(dir)
    tool.mkdir(dir .. "/tests")
    if not opts.cli then
        tool.mkdir(dir .. "/migrations")  -- CLI apps may not need a DB
    end

    -- Pick templates by runtime + mode (cli vs server)
    local ext = runtime == "js" and ".js" or ".lua"
    local app_template, test_template
    if opts.cli then
        app_template  = runtime == "js" and templates.js_cli_app  or templates.lua_cli_app
        test_template = runtime == "js" and templates.js_cli_test or templates.lua_cli_test
    else
        app_template  = runtime == "js" and templates.js_app  or templates.lua_app
        test_template = runtime == "js" and templates.js_test or templates.lua_test
    end

    tool.write_file(dir .. "/app" .. ext, app_template)
    tool.write_file(dir .. "/tests/test_app" .. ext, test_template)
    if not opts.cli then
        tool.write_file(dir .. "/migrations/001_init.sql", templates.migration_init)
    end
    tool.write_file(dir .. "/.gitignore", templates.gitignore)

    print("hull new: created " .. dir .. "/")
    print("  " .. dir .. "/app" .. ext)
    print("  " .. dir .. "/tests/test_app" .. ext)
    if not opts.cli then
        print("  " .. dir .. "/migrations/001_init.sql")
    end
    print("  " .. dir .. "/.gitignore")
    print("")
    print("Next steps:")
    print("  cd " .. dir)
    if opts.cli then
        print("  hull run app" .. ext .. " -- world")
    else
        print("  hull app" .. ext)
    end
end

main()
