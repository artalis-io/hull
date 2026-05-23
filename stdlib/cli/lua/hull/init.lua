--
-- hull.init — Initialize a Hull project in the current (or specified) directory
--
-- Usage: hull init [dir] [--runtime lua|js] [--cli]
--
-- Unlike `hull new`, init is idempotent: it creates missing files but never
-- overwrites existing ones. Works in-place like `git init`.
--
-- If an app file already exists, mode (CLI vs server) is detected from its
-- contents (presence of `app.main(`). Otherwise the --cli flag picks mode;
-- default is server.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

-- ── File templates (same content as hull.new) ────────────────────────

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
import { time } from "hull:time";

app.manifest({
    modules: [
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
    modules = {},
})

app.main(function(ctx)
    local who = ctx.args[1] or "world"
    ctx.stdout:write("hello " .. who .. "\n")
    return 0
end)
]]

templates.lua_cli_test = [[test("placeholder", function()
    test.eq(1, 1)
end)
]]

templates.js_cli_app = [[// CLI app — `hull run app.js [-- args...]` invokes app.main once and exits.
import { app } from "hull:app";

app.manifest({
    modules: [],
});

app.main(async (ctx) => {
    const who = ctx.args[0] ?? "world";
    ctx.stdout.write(`hello ${who}\n`);
    return 0;
});
]]

templates.js_cli_test = [[test("placeholder", () => {
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

-- ── Helpers ──────────────────────────────────────────────────────────

local function file_exists(path)
    return tool.file_exists(path)
end

local function ensure_dir(path)
    if not file_exists(path) then
        tool.mkdir(path)
        return true   -- created
    end
    return false      -- already existed
end

-- Write file only if it does not already exist. Returns true if written.
local function ensure_file(path, content)
    if not file_exists(path) then
        tool.write_file(path, content)
        return true
    end
    return false
end

local function detect_existing_runtime(dir)
    if file_exists(dir .. "/app.lua") then return "lua" end
    if file_exists(dir .. "/app.js")  then return "js"  end
    return nil
end

-- Detect mode from an existing app file: returns "cli" if app.main is
-- registered, otherwise "server". Returns nil if the file is missing or
-- unreadable.
local function detect_existing_mode(dir, runtime)
    if not runtime then return nil end
    local ext = (runtime == "js") and ".js" or ".lua"
    local path = dir .. "/app" .. ext
    if not file_exists(path) then return nil end
    local content = tool.read_file(path)
    if not content then return nil end
    if content:find("app%.main%s*%(", 1, false) then return "cli" end
    return "server"
end

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        dir     = ".",
        runtime = nil,   -- nil = auto-detect from existing files, else "lua"/"js"
        cli     = nil,   -- nil = auto-detect or default server; true = force CLI
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
            opts.dir = a
        end
        i = i + 1
    end

    return opts
end

-- ── Main ─────────────────────────────────────────────────────────────

local function main()
    local opts = parse_args()
    local dir  = opts.dir

    -- Validate runtime flag if given
    if opts.runtime and opts.runtime ~= "lua" and opts.runtime ~= "js" then
        tool.stderr("hull init: invalid runtime '" .. opts.runtime .. "' (use lua or js)\n")
        tool.exit(1)
    end

    -- Create target directory if needed
    local dir_created = false
    if not file_exists(dir) then
        tool.mkdir(dir)
        dir_created = true
    end

    -- Detect or choose runtime
    local existing_runtime = detect_existing_runtime(dir)
    local runtime = opts.runtime or existing_runtime or "lua"

    -- Detect or choose mode (CLI vs server). Priority:
    --   1. --cli flag (explicit opt-in)
    --   2. existing app file content (app.main → cli, else server)
    --   3. default: server
    local existing_mode = detect_existing_mode(dir, existing_runtime)
    local cli_mode = opts.cli or (existing_mode == "cli")

    local ext = runtime == "js" and ".js" or ".lua"
    local app_template, test_template
    if cli_mode then
        app_template  = runtime == "js" and templates.js_cli_app  or templates.lua_cli_app
        test_template = runtime == "js" and templates.js_cli_test or templates.lua_cli_test
    else
        app_template  = runtime == "js" and templates.js_app  or templates.lua_app
        test_template = runtime == "js" and templates.js_test or templates.lua_test
    end

    -- Track what we create vs skip
    local created = {}
    local skipped = {}

    local function track_file(path, content)
        if ensure_file(path, content) then
            created[#created + 1] = path
        else
            skipped[#skipped + 1] = path
        end
    end

    local function track_dir(path)
        if ensure_dir(path) then
            created[#created + 1] = path .. "/"
        end
    end

    if not cli_mode then
        track_dir(dir .. "/migrations")
    end
    track_dir(dir .. "/tests")

    track_file(dir .. "/app" .. ext,            app_template)
    track_file(dir .. "/tests/test_app" .. ext, test_template)
    if not cli_mode then
        track_file(dir .. "/migrations/001_init.sql", templates.migration_init)
    end
    track_file(dir .. "/.gitignore", templates.gitignore)

    -- Output
    local display_dir = (dir == ".") and "./" or dir .. "/"
    if dir_created then
        print("hull init: created " .. display_dir)
    else
        print("hull init: initialized " .. display_dir)
    end
    print("  runtime   " .. runtime)
    print("  mode      " .. (cli_mode and "cli" or "server"))
    print("")

    if #created > 0 then
        print("  created:")
        for _, f in ipairs(created) do
            print("    " .. f)
        end
    end

    if #skipped > 0 then
        print("  already present (unchanged):")
        for _, f in ipairs(skipped) do
            print("    " .. f)
        end
    end

    print("")

    local app_path = (dir == "." and "" or dir .. "/") .. "app" .. ext
    print("Next steps:")
    if cli_mode then
        print("  hull run " .. app_path .. " -- world")
    else
        print("  hull " .. app_path)
    end
end

main()
