--- ETag response helpers — `etag.json`, `etag.text`, `etag.html`.
--
-- Not a Keel middleware (no `(req, res) -> int` signature). Instead,
-- handlers call `etag.json(req, res, data)` in place of `res:json(data)`
-- and the helper:
--
--   1. Encodes the body.
--   2. Computes a weak ETag `W/"<first 16 hex of SHA-256>"`.
--   3. Returns `304 Not Modified` when `If-None-Match` matches.
--   4. Otherwise sends the full response with an `ETag` header.
--
-- Skips ETag work for:
--   - non-GET/HEAD methods
--   - bodies larger than 1 MB (`MAX_BODY_SIZE`)
--
-- @module hull.web.middleware.etag
-- @license AGPL-3.0-or-later
-- @usage
--   local etag = require("hull.web.middleware.etag")
--   app.get("/api/items", function(req, res)
--       etag.json(req, res, db.query("SELECT * FROM items"))
--   end)

local json = require("hull.json")
local crypto = require("hull.crypto")

local etag = {}

local MAX_BODY_SIZE = 1024 * 1024  -- 1 MB: skip ETag for larger responses

--- Compute a weak ETag from a body string.
--
-- @function etag.compute
-- @tparam string body
-- @treturn ?string  `W/"<16-hex>"`, or `nil` for empty/error.
function etag.compute(body)
    if not body or #body == 0 then return nil end
    local ok, hash = pcall(crypto.sha256, body)
    if not ok or not hash then return nil end
    return 'W/"' .. hash:sub(1, 16) .. '"'
end

--- Does the request's `If-None-Match` match `tag`?
--
-- Accepts a comma-separated list of values and the `*` wildcard.
--
-- @function etag.matches
-- @tparam table req
-- @tparam ?string tag
-- @treturn boolean
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
--
-- Returns `304 Not Modified` when `If-None-Match` matches; otherwise
-- sends the encoded body with an `ETag` header.
--
-- @function etag.json
-- @tparam table  req
-- @tparam table  res
-- @tparam any    data    Body data (JSON-encoded).
-- @tparam[opt=200] number status  HTTP status when sending the body.
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
-- @function etag.text
-- @tparam table  req
-- @tparam table  res
-- @tparam string text
-- @tparam[opt=200] number status
function etag.text(req, res, text, status)
    if not text then text = "" end
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
-- @function etag.html
-- @tparam table  req
-- @tparam table  res
-- @tparam string html
-- @tparam[opt=200] number status
function etag.html(req, res, html, status)
    if not html then html = "" end
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
