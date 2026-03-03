--
-- hull.form -- URL-encoded form body parsing
--
-- Pure function for decoding application/x-www-form-urlencoded strings.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local form = {}

--- Decode a percent-encoded byte (%XX) to its character.
-- Returns the original sequence if the hex digits are invalid.
local function decode_percent(hex)
    local n = tonumber(hex, 16)
    if n then return string.char(n) end
    return "%" .. hex
end

--- Parse a URL-encoded form body into a key-value table.
-- "email=a%40b.com&pass=hello+world" -> { email = "a@b.com", pass = "hello world" }
-- Last value wins for duplicate keys. Returns {} for nil/empty/non-string input.
function form.parse(body)
    local result = {}
    if not body or type(body) ~= "string" or body == "" then
        return result
    end

    for pair in body:gmatch("[^&]+") do
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
