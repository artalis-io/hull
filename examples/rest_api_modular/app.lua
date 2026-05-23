-- Modular REST API scaffold.
--
-- This file is just the bootstrap: declare the manifest, initialise
-- session/middleware modules that need it, and mount each route group.
-- Add a new resource by creating routes/<name>.lua + models/<name>.lua
-- and requiring its `register(app)` here.

-- Declare every first-party module the app + subfiles import. The
-- runtime tracker validates this list against actual imports at load
-- time, so missing entries surface immediately. `hull/middleware/session`
-- and friends auto-pull their own deps (json, time, crypto, db) via
-- the registry's deps graph — but anything imported only by user files
-- (routes/, models/, lib/) must be listed here explicitly.
app.manifest({
    modules = {
        "hull/http-server@1",
        "hull/middleware/logger@1",
        "hull/log@1",
        "hull/db@1",           -- models/user.lua
        "hull/crypto@1",       -- models/user.lua (random id)
        "hull/time@1",         -- models/user.lua (created_at)
        "hull/validate@1",     -- lib/validate_user.lua
        "hull/json@1",         -- routes/users.lua (request body decode)
    },
})

local log    = require("hull.log")
local logger = require("hull.middleware.logger")

-- Cross-cutting middleware first: request logging on every path.
app.use("*", "/*", logger.middleware({}))

-- Mount each route group. Add new groups below as the app grows.
require("./routes/health").register(app)
require("./routes/users").register(app)

log.info("rest api loaded")
