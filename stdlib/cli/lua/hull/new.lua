--
-- hull.new — Scaffold a new Hull project
--
-- Usage: hull new <name> [--runtime lua|js] [--cli] [--type TYPE]
--
-- Layout types (--type):
--   flat (default)  — single app.lua + tests/ + migrations/. Best for
--                     small services, demos, single-file experiments.
--   rest            — modular REST API: app.lua + routes/ + middleware/
--                     + models/ + lib/. Best for apps that will grow
--                     past one resource.
--   cli             — modular CLI tool: app.lua + commands/ + lib/.
--                     (Planned — Stage 2 of the layout work.)
--   tui             — modular TUI app: app.lua + views/ + lib/.
--                     (Planned — Stage 3 of the layout work.)
--
-- The legacy --cli flag (no --type) remains a flat single-file CLI
-- and is unchanged. --type takes precedence when both are given.
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
        layout = nil,  -- nil → derived from --cli/default; set by --type
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--runtime" then
            i = i + 1
            opts.runtime = arg[i]
        elseif a == "--cli" then
            opts.cli = true
        elseif a == "--type" then
            i = i + 1
            opts.layout = arg[i]
        elseif a:sub(1, 1) ~= "-" then
            opts.name = a
        end
        i = i + 1
    end

    -- Resolve layout: explicit --type wins; --cli implies flat-cli;
    -- otherwise default to flat (single-file server).
    if not opts.layout then
        opts.layout = opts.cli and "flat-cli" or "flat"
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

    local known_layouts = { ["flat"] = true, ["flat-cli"] = true,
                            ["rest"] = true, ["cli"] = true, ["tui"] = true }
    if not known_layouts[opts.layout] then
        tool.stderr("hull new: invalid --type '" .. opts.layout ..
                    "' (use flat, rest, cli, or tui)\n")
        tool.exit(1)
    end

    -- Check if directory already exists
    if tool.file_exists(opts.name) then
        tool.stderr("hull new: directory '" .. opts.name .. "' already exists\n")
        tool.exit(1)
    end

    local dir = opts.name
    local ext = runtime == "js" and ".js" or ".lua"

    -- Modular layouts delegate to a per-type templates module. Each
    -- module's M.files(runtime) returns either:
    --   (table, nil)  — { ["relative/path"] = "content" } to write
    --   (nil, "msg")  — runtime not supported (e.g. TUI rejects JS)
    -- This function creates parent dirs as needed, writes files,
    -- and prints a tree of what was created.
    local MODULAR_TYPES = {
        rest = { module = "hull.templates_rest", label = "modular REST API",  default = "hull app" },
        cli  = { module = "hull.templates_cli",  label = "modular CLI tool",  default = "hull run app -- greet world" },
        tui  = { module = "hull.templates_tui",  label = "modular TUI app",   default = "hull run app" },
    }
    local mt = MODULAR_TYPES[opts.layout]
    if mt then
        local templates_mod = require(mt.module)
        local files, err    = templates_mod.files(runtime)
        if err then
            tool.stderr("hull new: " .. err .. "\n")
            tool.exit(1)
        end

        tool.mkdir(dir)
        local created = {}
        for path, content in pairs(files) do
            local full = dir .. "/" .. path
            local last_slash = full:find("/[^/]*$")
            if last_slash then
                local parent = full:sub(1, last_slash - 1)
                tool.mkdir(parent)
            end
            tool.write_file(full, content)
            created[#created + 1] = full
        end
        table.sort(created)

        print("hull new: created " .. dir .. "/ (" .. mt.label .. ")")
        for _, p in ipairs(created) do print("  " .. p) end
        print("")
        print("Next steps:")
        print("  cd " .. dir)
        print("  " .. (mt.default:gsub("app", "app" .. ext)))
        return
    end

    -- Flat layouts (default and --cli) keep their original behavior.
    tool.mkdir(dir)
    tool.mkdir(dir .. "/tests")
    if opts.layout == "flat" then
        tool.mkdir(dir .. "/migrations")  -- CLI apps may not need a DB
    end

    local app_template, test_template
    if opts.layout == "flat-cli" then
        app_template  = runtime == "js" and templates.js_cli_app  or templates.lua_cli_app
        test_template = runtime == "js" and templates.js_cli_test or templates.lua_cli_test
    else
        app_template  = runtime == "js" and templates.js_app  or templates.lua_app
        test_template = runtime == "js" and templates.js_test or templates.lua_test
    end

    tool.write_file(dir .. "/app" .. ext, app_template)
    tool.write_file(dir .. "/tests/test_app" .. ext, test_template)
    if opts.layout == "flat" then
        tool.write_file(dir .. "/migrations/001_init.sql", templates.migration_init)
    end
    tool.write_file(dir .. "/.gitignore", templates.gitignore)

    print("hull new: created " .. dir .. "/")
    print("  " .. dir .. "/app" .. ext)
    print("  " .. dir .. "/tests/test_app" .. ext)
    if opts.layout == "flat" then
        print("  " .. dir .. "/migrations/001_init.sql")
    end
    print("  " .. dir .. "/.gitignore")
    print("")
    print("Next steps:")
    print("  cd " .. dir)
    if opts.layout == "flat-cli" then
        print("  hull run app" .. ext .. " -- world")
    else
        print("  hull app" .. ext)
    end
end

main()
