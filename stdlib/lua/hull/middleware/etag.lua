--
-- hull.middleware.etag -- ETag response helpers
--
-- Wraps res:json/text/html to add ETag support with 304 Not Modified.
-- Not a middleware function (runs after handler, not before) — instead
-- provides wrapper functions that replace res:json() in route handlers.
--
-- Usage:
--   local etag = require("hull.middleware.etag")
--   app.get("/api/items", function(req, res)
--       local data = db.query("SELECT * FROM items")
--       etag.json(req, res, data)
--   end)
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local etag = {}

local MAX_BODY_SIZE = 1024 * 1024  -- 1 MB: skip ETag for larger responses

--- Compute a weak ETag from a body string.
-- Returns W/"<first 16 hex chars of SHA-256>"
function etag.compute(body)
    if not body or #body == 0 then return nil end
    local hash = crypto.sha256(body)
    return 'W/"' .. hash:sub(1, 16) .. '"'
end

--- Check if the request's If-None-Match header matches an ETag value.
-- Handles comma-separated lists and * wildcard.
function etag.matches(req, tag)
    if not tag then return false end
    local inm = req.headers["if-none-match"]
    if not inm then return false end

    -- Wildcard
    if inm:match("^%s*%*%s*$") then return true end

    -- Check each comma-separated value
    for part in inm:gmatch("[^,]+") do
        local trimmed = part:match("^%s*(.-)%s*$")
        if trimmed == tag then return true end
    end
    return false
end

--- Send a JSON response with ETag support.
-- If If-None-Match matches, sends 304 (no body).
-- Otherwise sends full response with ETag header.
-- status: optional HTTP status code (default 200)
function etag.json(req, res, data, status)
    -- Only compute ETag for GET/HEAD
    if req.method ~= "GET" and req.method ~= "HEAD" then
        if status then res:status(status) end
        res:json(data)
        return
    end

    local body = json.encode(data)
    if #body > MAX_BODY_SIZE then
        if status then res:status(status) end
        res:json(data)
        return
    end

    local tag = etag.compute(body)
    if etag.matches(req, tag) then
        res:status(304):header("ETag", tag)
        return
    end

    res:header("ETag", tag)
    if status then res:status(status) end
    res:json(data)
end

--- Send a text response with ETag support.
function etag.text(req, res, text, status)
    if req.method ~= "GET" and req.method ~= "HEAD" then
        if status then res:status(status) end
        res:text(text)
        return
    end

    if #text > MAX_BODY_SIZE then
        if status then res:status(status) end
        res:text(text)
        return
    end

    local tag = etag.compute(text)
    if etag.matches(req, tag) then
        res:status(304):header("ETag", tag)
        return
    end

    res:header("ETag", tag)
    if status then res:status(status) end
    res:text(text)
end

--- Send an HTML response with ETag support.
function etag.html(req, res, html, status)
    if req.method ~= "GET" and req.method ~= "HEAD" then
        if status then res:status(status) end
        res:html(html)
        return
    end

    if #html > MAX_BODY_SIZE then
        if status then res:status(status) end
        res:html(html)
        return
    end

    local tag = etag.compute(html)
    if etag.matches(req, tag) then
        res:status(304):header("ETag", tag)
        return
    end

    res:header("ETag", tag)
    if status then res:status(status) end
    res:html(html)
end

return etag
