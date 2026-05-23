-- models/user.lua — Persistence layer for the User resource.
--
-- One file per resource keeps SQL in one place and lets routes/users.lua
-- stay focused on HTTP concerns. The returned table is the resource's
-- public surface; route handlers shouldn't touch `db` directly.

local db     = require("hull.db")
local crypto = require("hull.crypto")
local time   = require("hull.time")

local M = {}

function M.create(input)
    -- 32 random bytes → SHA-256 hex (64 chars). crypto.random returns
    -- raw bytes; sha256 returns a hex string. Combined this gives a
    -- cryptographically-strong opaque id with no separate hex-encoder.
    local id  = crypto.sha256(crypto.random(32))
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
