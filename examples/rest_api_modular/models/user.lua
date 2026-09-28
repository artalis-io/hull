-- models/user.lua - Persistence layer for the User resource.
--
-- One file per resource keeps SQL in one place and lets routes/users.lua
-- stay focused on HTTP concerns. The returned table is the resource's
-- public surface; route handlers shouldn't touch `db` directly.

local db     = require("hull.db").default()
local crypto = require("hull.crypto")
local time   = require("hull.time")

local M = {}

function M.create(input)
    -- 32 random bytes as hex (64 chars): a cryptographically-strong
    -- opaque id.
    local id  = crypto.random_token(32, "hex")
    local now = time.now()
    db.exec(
        "INSERT INTO users (id, email, name, created_at) VALUES (?, ?, ?, ?)",
        { id, input.email, input.name, now }
    )
    return M.find_by_id(id)
end

function M.find_by_id(id)
    local rows = db.query(
        "SELECT id, email, name, created_at FROM users WHERE id = ?",
        { id }
    )
    return rows[1]
end

function M.list(opts)
    opts = opts or {}
    local limit = opts.limit or 50
    return db.query(
        "SELECT id, email, name, created_at FROM users " ..
        "ORDER BY created_at DESC LIMIT ?",
        { limit }
    )
end

function M.delete_by_id(id)
    local rows = db.query("SELECT id FROM users WHERE id = ?", { id })
    if #rows == 0 then return false end
    db.exec("DELETE FROM users WHERE id = ?", { id })
    return true
end

return M
