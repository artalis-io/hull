--
-- hull.new — Scaffold a new Hull project
--
-- Usage: hull new <name> [--runtime lua|js]
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

-- ── Templates ────────────────────────────────────────────────────────

local templates = {}

templates.lua_app = [[app.get("/", function(_req, res)
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

templates.js_app = [[app.get("/", (_req, res) => {
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

templates.gitignore = [[data.db
data.db-*
*.key
hull.sig
build/
]]

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        name = nil,
        runtime = "lua",
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--runtime" then
            i = i + 1
            opts.runtime = arg[i]
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
        tool.stderr("Usage: hull new <name> [--runtime lua|js]\n")
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

    -- Write app file
    local ext = runtime == "js" and ".js" or ".lua"
    local app_template = runtime == "js" and templates.js_app or templates.lua_app
    local test_template = runtime == "js" and templates.js_test or templates.lua_test

    tool.write_file(dir .. "/app" .. ext, app_template)
    tool.write_file(dir .. "/tests/test_app" .. ext, test_template)
    tool.write_file(dir .. "/.gitignore", templates.gitignore)

    print("hull new: created " .. dir .. "/")
    print("  " .. dir .. "/app" .. ext)
    print("  " .. dir .. "/tests/test_app" .. ext)
    print("  " .. dir .. "/.gitignore")
    print("")
    print("Next steps:")
    print("  cd " .. dir)
    print("  hull app" .. ext)
end

main()
