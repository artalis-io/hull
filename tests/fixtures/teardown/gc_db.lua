-- app.main whose __gc finalizers use the database while lua_close runs them
-- at teardown (tests/e2e_teardown.sh). The app context freed the connection
-- registry - which a borrowed connection object points into - before the
-- runtime, so each of these read an unmapped page (audit 17). Now every one
-- must fail with an error, and the run must exit 0.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
local dbmod = require("hull.db")
local log = require("hull.log")

app.manifest({
    modules = { "hull/db@1", "hull/log@1" },
    databases = { dynamic = { schemes = { "sqlite" } } },
})

local conn = dbmod.default()

local function try(fn)
    local ok = pcall(fn)
    return ok and "ran" or "refused"
end

-- Collected only by lua_close, after the registry's connections are closed.
local at_close = setmetatable({}, { __gc = function()
    log.warn("teardown-gc: conn=" .. try(function() conn.query("SELECT 1") end)
        .. " default=" .. try(function() dbmod.default().query("SELECT 1") end)
        .. " connect=" .. try(function() dbmod.connect("default") end)
        .. " open=" .. try(function() dbmod.open(":memory:").close() end))
end })

app.main(function(ctx)
    local _ = at_close   -- keeps it alive until the runtime goes
    conn.exec("CREATE TABLE IF NOT EXISTS t (x INTEGER)")
    ctx.stdout:write("teardown-gc: main ran " .. #conn.query("SELECT 1 AS v") .. "\n")
    return 0
end)
