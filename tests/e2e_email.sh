#!/bin/sh
# e2e_email.sh: hull.email error-convention + Lua/JS parity.
#
# email.send follows the stdlib error convention (docs/stdlib_style.md section
# 1): a failure to send THROWS a coded error (a { code, message } table in Lua,
# an Error with .code in JS); success returns true. This runs the full
# validation branch matrix through the real embedded module in BOTH runtimes and
# asserts the emitted .code values are exact and identical, so a regression back
# to {ok=false} - or a code drift - fails CI. All cases throw before any network
# I/O, so no live server / host allowlist is needed.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/email.lua" <<'LUA'
local email = require("hull.email")
app.manifest({ modules = { "hull/email@1" }, hosts = {} })
local function code(opts)
    local ok, err = pcall(email.send, opts)
    if ok then return "OK" end
    if type(err) == "table" and err.code then return err.code end
    return "ERR"
end
app.main(function(ctx)
    local out = {
        code(nil),
        code({ to = "a@b.com", subject = "s", body = "b" }),
        code({ from = "a@b.com", subject = "s", body = "b" }),
        code({ from = "a@b.com", to = "c@d.com", body = "b" }),
        code({ from = "a@b.com", to = "c@d.com", subject = "s" }),
        code({ from = "bad", to = "c@d.com", subject = "s", body = "b" }),
        code({ from = "a@b.com", to = "bad", subject = "s", body = "b" }),
        code({ provider = "nope", from = "a@b.com", to = "c@d.com", subject = "s", body = "b" }),
        code({ provider = "postmark", from = "a@b.com", to = "c@d.com", subject = "s", body = "b" }),
    }
    ctx.stdout:write(table.concat(out, "|") .. "\n"); return 0
end)
LUA

cat > "$WD/email.js" <<'JS'
import { app } from "hull:app";
import { email } from "hull:email";
app.manifest({ modules: ["hull/email@1"], hosts: [] });
async function code(opts) {
    try { await email.send(opts); return "OK"; }
    catch (e) { return e.code || "ERR"; }
}
app.main(async (ctx) => {
    const out = [];
    out.push(await code(undefined));
    out.push(await code({ to: "a@b.com", subject: "s", body: "b" }));
    out.push(await code({ from: "a@b.com", subject: "s", body: "b" }));
    out.push(await code({ from: "a@b.com", to: "c@d.com", body: "b" }));
    out.push(await code({ from: "a@b.com", to: "c@d.com", subject: "s" }));
    out.push(await code({ from: "bad", to: "c@d.com", subject: "s", body: "b" }));
    out.push(await code({ from: "a@b.com", to: "bad", subject: "s", body: "b" }));
    out.push(await code({ provider: "nope", from: "a@b.com", to: "c@d.com", subject: "s", body: "b" }));
    out.push(await code({ provider: "postmark", from: "a@b.com", to: "c@d.com", subject: "s", body: "b" }));
    ctx.stdout.write(out.join("|") + "\n"); return 0;
});
JS

lua_out="$("$HULL" "$WD/email.lua" 2>/dev/null | tail -1)"
js_out="$("$HULL" "$WD/email.js"  2>/dev/null | tail -1)"

# nil opts / missing from / to / subject / body / bad-from / bad-to =>
# invalid_argument; unknown provider => unknown_provider; missing api_key =>
# invalid_argument.
expect="invalid_argument|invalid_argument|invalid_argument|invalid_argument|invalid_argument|invalid_argument|invalid_argument|unknown_provider|invalid_argument"

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: hull.email throws coded errors, byte-identical across Lua and JS"
else
    echo "::error hull.email error-convention DRIFT:"
    echo "  expect=$expect"
    echo "  lua   =$lua_out"
    echo "  js    =$js_out"
    exit 1
fi
