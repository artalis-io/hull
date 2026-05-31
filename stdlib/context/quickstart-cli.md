<!-- minimal -->
## Quickstart: CLI tool

For non-web apps (one-shot scripts, batch processors, daemons), use
`app.main(fn)` instead of route handlers. The handler runs once, gets
a context with `args`/`env`/`stdin`/`stdout`/`stderr`, and its return
value is the process exit code.

```bash
hull init mytool --runtime=lua
cd mytool
# edit app.lua (see below)
hull dev                          # iterate
hull build .                      # → build/mytool (signed single binary)
./build/mytool --help             # use it
```

### Minimal CLI app

```lua
-- app.lua
app.manifest({
    modules = { "hull/log@1" },   -- no http-server needed
})

app.main(function(ctx)
    if #ctx.args == 0 then
        ctx.stderr:write("usage: mytool <name>\n")
        return 2
    end
    log.info("hello, " .. ctx.args[1])
    return 0
end)
```

Run with `hull dev` (or build + invoke). Exit code is whatever
`app.main` returns; non-zero = error.

### Output to stdout

```lua
app.main(function(ctx)
    local result = { ok = true, count = 42 }
    ctx.stdout:write(json.encode(result))
    ctx.stdout:write("\n")
    return 0
end)
```

`json` is a Hull intrinsic — no `manifest.modules` entry needed.

<!-- compact -->
## The CLI build flavor

If your tool doesn't need an HTTP server, build with
`HL_ENABLE_HTTP_SERVER=0`. Binary drops ~600 KB; modules
`hull/server`, `hull/ws`, `hull/web/sse`, every `hull/middleware/*` are
unavailable at module-resolve time.

```bash
make HL_ENABLE_HTTP_SERVER=0 EMBED_PLATFORM=1
# then hull build . inherits the smaller platform
```

For client-only CLIs (call APIs, no listener), pure compute tools,
etc., see `docs/cli_mode.md`.

## Reading stdin

```lua
app.main(function(ctx)
    local input = ctx.stdin:read("*a")    -- entire stdin to string
    local parsed = json.decode(input)
    -- … do work …
    ctx.stdout:write(json.encode(parsed))
    return 0
end)
```

`ctx.stdin:read("*l")` for one line; `ctx.stdin:read(n)` for n bytes.

## Async operations work in main

`app.main` runs on the event loop, so:

```lua
app.main(function(ctx)
    local res, err = http.fetch("https://api.example.com/data")
    if err then return 1 end
    log.info("got " .. #res.body .. " bytes")

    -- WASM compute, GPU dispatch, hull.sleep — all valid here
    local out = compute.async.call("transform", res.body)
    log.info(out.result)
    return 0
end)
```

`http.fetch` requires `manifest.hosts = {"api.example.com"}`. See
`hull agent context --task=compute` for WASM, `--task=gpu` for GPU.

## Combining CLI + server

`app.main` and route registration coexist. `main` runs as a startup
hook (warm caches, run migrations, prefetch config), THEN serve loop
runs. If `main` returns non-zero, the serve loop is skipped.

```lua
app.main(function(ctx) -- runs once at boot
    log.info("warming caches…")
    cache.preload()
    return 0   -- nil/0 → continue to serve
end)

app.get("/", function(req, res) ... end)   -- normal routes
```

## Testing CLI apps

```lua
-- tests/test_main.lua
local t = require("hull.test")

t.case("main with no args exits 2", function()
    local rc = t.run_main({})
    t.assert_eq(rc, 2)
end)

t.case("main echoes name", function()
    local stdout = {}
    local rc = t.run_main({"alice"}, { stdout = stdout })
    t.assert_eq(rc, 0)
    t.assert_match(table.concat(stdout), "alice")
end)
```

<!-- full -->
## Argv parsing

Hull doesn't ship an argparse module — keep it minimal:

```lua
app.main(function(ctx)
    local opts = { verbose = false, output = nil }
    local positional = {}
    local i = 1
    while i <= #ctx.args do
        local a = ctx.args[i]
        if a == "--verbose" or a == "-v" then
            opts.verbose = true
        elseif a == "--output" or a == "-o" then
            i = i + 1
            opts.output = ctx.args[i]
        elseif a:sub(1,1) == "-" then
            ctx.stderr:write("unknown flag: " .. a .. "\n")
            return 2
        else
            positional[#positional + 1] = a
        end
        i = i + 1
    end
    -- … do work using opts + positional …
    return 0
end)
```

For non-trivial CLIs, declare subcommands explicitly:

```lua
local commands = {
    list   = function(ctx, args) ... end,
    add    = function(ctx, args) ... end,
    remove = function(ctx, args) ... end,
}

app.main(function(ctx)
    local cmd = ctx.args[1]
    if not cmd or not commands[cmd] then
        ctx.stderr:write("usage: mytool <list|add|remove> [args]\n")
        return 2
    end
    return commands[cmd](ctx, { table.unpack(ctx.args, 2) })
end)
```

## Reading environment variables

```lua
app.manifest({
    modules = { "hull/log@1" },
    env = { "HOME", "MY_CONFIG_PATH" },   -- allowlist
})

app.main(function(ctx)
    local home = env.get("HOME")          -- only declared vars work
    local cfg  = env.get("MY_CONFIG_PATH")
    -- …
end)
```

Undeclared env access throws at call time. See
`hull agent context --task=validation`.

## Writing files

```lua
app.manifest({
    modules = { "hull/fs@1" },
    fs = { write = { "/tmp/", "./output/" } },
})

app.main(function(ctx)
    fs.write("./output/result.json", json.encode(data))
    return 0
end)
```

`fs.write` is restricted to the declared prefixes; path traversal is
rejected. `fs.read` works the same.

## Cron-style scheduling inside the binary

If the tool should run periodically:

```lua
app.every(60_000, function() do_periodic_work() end)
-- or
app.daily("02:30", function() nightly_cleanup() end)
```

The binary stays running. For pure cron (system-managed), let your
CLI exit and schedule it externally.

## Distribution

```bash
hull build .                              # local: build/mytool
hull build . --target=x86_64              # cross-compile WASM AOT
hull build . --compiler=tcc               # use embedded TinyCC (no system cc)
```

Tools ship as one signed binary. Users install with `cp` or
`install`; no runtime dependencies.

## See also

- `hull agent context --task=quickstart-web` — server-side variant
- `hull agent context --task=quickstart-tui` — terminal UI variant
- `hull agent context --task=build` — build options + cross-compile
- `hull agent context --task=compute` — WASM modules for heavy lifting
- `docs/cli_mode.md` — when to drop HTTP entirely (HL_ENABLE_HTTP_SERVER=0)
