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

-- The real table behind a stdlib module, for a test that stubs what the
-- stdlib calls (http-client's async.post): app code, the tests included,
-- gets a read-only view of every module since audit 43. Required here by
-- name, from this stdlib chunk, so the result is the stdlib's own table.
hooks["hull._test_hooks"] = {
    real = function(name)
        return require(name)
    end,
}

return M
