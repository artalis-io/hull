--
-- hull.templates_ts - the TypeScript scaffolds shared by `hull new --runtime
-- ts` and `hull init --runtime ts` (docs/typescript_design.md §4.5).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

M.app = [[// Declare every first-party module the app imports.
// The runtime gate refuses undeclared imports - `hull modules available`
// lists the full registry. Add capability sections (fs, hosts, env)
// alongside `modules` when a module needs them.
//
// Hull runs this file by erasing its types; it does not type-check it.
// Check it with tsc (or tsgo): `tsc --noEmit -p .`
import { app }  from "hull:app";
import { log }  from "hull:log";
import { time } from "hull:time";

interface Health {
    status: "ok";
    uptime: number;
}

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
    const body: Health = { status: "ok", uptime: time.clock() };
    res.json(body);
});

log.info("app loaded");
]]

M.test = [[test("GET / returns ok", async () => {
    const res = await test.get("/");
    test.eq(res.status, 200);
});

test("GET /health returns ok", async () => {
    const res = await test.get("/health");
    test.eq(res.status, 200);
    test.ok(res.json.uptime);
});
]]

M.cli_app = [[// CLI app - `hull run app.ts [-- args...]` invokes app.main once and exits.
// `app.main` is mutually exclusive with route registration: pick CLI mode
// (app.main) or server mode (app.get/etc), not both.
//
// Hull runs this file by erasing its types; it does not type-check it.
// Check it with tsc (or tsgo): `tsc --noEmit -p .`

import { app } from "hull:app";

app.manifest({
    // Declare every first-party module the app imports.
    modules: [],
    // env: ["HOME", "LANG"],   // env vars main may read via ctx.env
});

function greeting(who: string): string {
    return `hello ${who}\n`;
}

app.main(async (ctx) => {
    // ctx.args   : Array<string> of argv passed after `--`
    // ctx.env    : Object of env-vars allowed by manifest.env
    // ctx.stdin  : read(...), close()
    // ctx.stdout : write(...), flush()
    // ctx.stderr : write(...), flush()

    ctx.stdout.write(greeting(ctx.args[0] ?? "world"));
    return 0;  // exit code (undefined → 0, integer clamped to 0..255)
});
]]

M.cli_test = [[// test.runMain(args, opts) runs app.main once with ctx.args = args and
// returns { code, stdout, stderr } (ctx.stdout / ctx.stderr captured).
// opts: stdin (string or bytes), env (names from manifest.env), timeoutMs.

test("greets the named arg", () => {
    const r = test.runMain(["alice"]);
    test.eq(r.code, 0);
    test.eq(r.stdout, "hello alice\n");
});

test("greets the world by default", () => {
    test.eq(test.runMain([]).stdout, "hello world\n");
});
]]

-- A layout without TypeScript templates of its own is its JavaScript files
-- under .ts names: every ".js" in a path or in the text (relative import
-- specifiers, the CLI's dynamic import, comments naming a sibling file)
-- becomes ".ts". JavaScript is TypeScript, and the tsconfig `hull types`
-- writes leaves implicit any off, so they type-check as they are.
function M.from_js_text(content)
    return (content:gsub("%.js%f[^%w]", ".ts"))
end

function M.from_js(files)
    local out = {}
    for path, content in pairs(files) do
        out[(path:gsub("%.js$", ".ts"))] = M.from_js_text(content)
    end
    return out
end

-- The declarations and tsconfig.json, as `hull types <dir>` writes them.
function M.install_types(dir)
    local types_main = require("hull.types")
    types_main({ dir = dir, quiet = true })
end

return M
