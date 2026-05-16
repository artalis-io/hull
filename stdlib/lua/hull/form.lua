--- URL-encoded form-body parsing.
--
-- @module hull.form
-- @license AGPL-3.0-or-later

local form = {}

--- Decode a percent-encoded byte (%XX) to its character.
-- Returns the original sequence if the hex digits are invalid.
local function decode_percent(hex)
    local n = tonumber(hex, 16)
    if n then return string.char(n) end
    return "%" .. hex
end

--- Parse a URL-encoded form body into a key-value table.
--
-- Handles `+` → space and `%XX` percent-decoding. Last value wins for
-- duplicate keys. Empty/`nil`/non-string input returns an empty table
-- (no error).
--
-- @tparam string body  Body bytes (typically `req.body` for a POST
--   `application/x-www-form-urlencoded` request).
-- @tparam[opt] table opts  Options:
--
--   - `max_fields` (integer, default `1000`): hard cap on the number
--     of `&`-separated pairs; exceeding raises an error.
--
-- @treturn {[string]=string} Parsed fields.
-- @raise If `body` contains more than `opts.max_fields` pairs.
-- @usage
-- local fields = form.parse(req.body)
-- local email = fields.email
function form.parse(body, opts)
    local result = {}
    if not body or type(body) ~= "string" or body == "" then
        return result
    end

    local max_fields = (opts and opts.max_fields) or 1000
    local field_count = 0
    for pair in body:gmatch("[^&]+") do
        field_count = field_count + 1
        if field_count > max_fields then
            error("form.parse: exceeded max_fields limit (" .. max_fields .. ")")
        end
        local eq = pair:find("=", 1, true)
        if eq then
            local key = pair:sub(1, eq - 1)
            local value = pair:sub(eq + 1)

            -- Skip empty keys
            if key ~= "" then
                -- Decode + to space, then percent-encoded bytes
                key = key:gsub("+", " "):gsub("%%(%x%x)", decode_percent)
                value = value:gsub("+", " "):gsub("%%(%x%x)", decode_percent)
                result[key] = value
            end
        end
    end

    return result
end

return form
