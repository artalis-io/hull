-- hull.db._internal - the connection the stdlib keeps its own _hull_* tables
-- on (stdlib-only: the underscore segment keeps it out of app code).
--
-- With `databases.internal` declared in the manifest it is that database,
-- reached under a role the app's own connection has no grants on, so the
-- separation is enforced by the database rather than by Hull's SQL-text
-- check. Without it, the default connection, as before.
--
-- A module takes its connection when it loads, which is before the manifest
-- is wired, so this is a proxy: every access asks for the connection afresh
-- until the manifest is wired, and caches it from then on. Method calls go
-- straight to the real connection's functions (`db.exec(sql, params)`), so the
-- _hull_* caller check sees the stdlib module that made the call.

local native = require("hull.db._internal_conn")

local M = {}

local cached

local function resolve()
    if cached then return cached end
    local conn, final = native.connection()
    if final then cached = conn end
    return conn
end

--- The internal connection, as a proxy usable at module load.
function M.connection()
    return setmetatable({}, {
        __index = function(_, k) return resolve()[k] end,
        __newindex = function() error("hull.db._internal: read-only", 2) end,
    })
end

return M
