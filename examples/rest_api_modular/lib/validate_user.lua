-- lib/validate_user.lua - Schema validation for the User resource.
--
-- Wraps `hull.validate` with per-resource schemas so routes/users.lua
-- can call `validate.create(body)` instead of inlining the rule table.
-- Adding a new field is a one-line schema change here.

local validate = require("hull.validate")

local M = {}

function M.create(body)
    return validate.check(body, {
        email = { required = true, type = "string", email = true, max = 254 },
        name  = { required = true, type = "string", trim = true, min = 1, max = 100 },
    })
end

return M
