-- middleware/require_auth.lua — App-specific auth wrapper.
--
-- This is where app-specific authentication policy lives: the stdlib
-- module `hull.web.middleware.auth` provides the session-cookie / JWT
-- primitives; this wrapper composes them with app conventions like
-- "redirect to /login instead of 401" or "always require a verified
-- email."
--
-- Empty by default — uncomment and adapt when you add login.

-- local auth = require("hull.web.middleware.auth")
--
-- local M = {}
--
-- function M.require_user()
--     return auth.session_middleware({
--         login_path = "/login",
--     })
-- end
--
-- return M

return {}
