-- hull._test_hooks - the stdlib modules' test hooks (internal: the underscore
-- keeps it out of app code).
--
-- A module's test-only helpers reach its private state (decrypt a stored TOTP
-- secret, un-mark a processed inbox message, reset its config) and run in a
-- stdlib frame, so the _hull_* table check admits their SQL. Exported on the
-- module itself (`totp._test`), any app could call them (audit 16). A module
-- registers them here instead, under its own require name. Only stdlib code
-- can require this module; the C unit-test harness reads it through the
-- runtime's trusted require. An app, under `hull test` too, has no way in.

local M = {}

local hooks = {}

function M.register(name, t)
    hooks[name] = t
end

function M.get(name)
    return hooks[name]
end

return M
