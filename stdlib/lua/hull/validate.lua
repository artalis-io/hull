--- Schema-based input validation for form / JSON / query data.
--
-- @module hull.validate
-- @license AGPL-3.0-or-later
--
-- @usage
-- local validate = require("hull.validate")
-- local ok, errors = validate.check(req.json, {
--     email = { required = true, email = true },
--     name  = { required = true, trim = true, min = 1, max = 100 },
--     age   = { type = "integer", min = 0, max = 150 },
-- })
-- if not ok then return res:status(422):json({ errors = errors }) end

local validate = {}

-- Email validation: practical RFC-5322 subset suitable for form-input
-- screening. Catches obvious garbage (a..b@x, @foo, user@, foo) without
-- aiming for full compliance — apps wanting stricter checks should layer
-- their own validator on top.
--
-- Constraints encoded in the pattern:
--   * local part starts with alphanumeric
--   * local part allows letters, digits, and . _ % + -
--   * single '@' boundary
--   * domain starts with alphanumeric
--   * domain has at least one '.'
--   * TLD ≥ 2 letters
local EMAIL_PATTERN =
    "^[A-Za-z0-9][A-Za-z0-9._+%-]*@[A-Za-z0-9][A-Za-z0-9.%-]*%.[A-Za-z][A-Za-z]+$"

-- Reject "..", trailing ".", and leading "." in local or domain — the
-- pattern above doesn't catch these. We do a secondary check at use time.
local function email_ok(s)
    if type(s) ~= "string" then return false end
    if #s > 254 then return false end
    if not s:match(EMAIL_PATTERN) then return false end
    if s:find("%.%.", 1, false) then return false end
    if s:find("^%.") or s:find("%.@") or s:find("@%.") then return false end
    return true
end

--- Validate a data table against a schema.
--
-- Schema is a table mapping field names to rule tables. Recognised rules:
--
--   - `required` (boolean) — error if absent.
--   - `trim` (boolean) — strip leading/trailing whitespace before other checks.
--   - `type` (string) — `"string"` / `"number"` / `"integer"` / `"boolean"`.
--   - `min` / `max` (number) — string length (for strings) or numeric bound (for numbers).
--   - `pattern` (string) — Lua pattern the value must match.
--   - `oneof` (`{any,...}`) — value must equal one of the listed values.
--   - `email` (boolean) — practical RFC-5322 subset (good for form screening).
--   - `fn` (`function(value) -> boolean`) — custom validator.
--   - `message` (string) — override the default error message.
--
-- @tparam table data    Input. Non-table input is treated as `{}`.
--   **NOTE:** when any field uses `trim = true`, `data[field]` is
--   replaced with the trimmed value in-place — the caller's table is
--   mutated. Pass a shallow copy if you need the original preserved.
-- @tparam table schema  Rules. Non-table input → `true, nil`.
-- @treturn boolean ok   `true` if all fields pass.
-- @treturn ?table errors  Maps field-name → error message. `nil` on success.
function validate.check(data, schema)
    if type(data) ~= "table" then data = {} end
    if type(schema) ~= "table" then return true, nil end

    local errors = nil

    for field, rules in pairs(schema) do
        local value = data[field]
        local err = nil
        local custom_msg = rules.message

        -- 1. trim (mutates data[field] in-place — caller's table is modified)
        if rules.trim and type(value) == "string" then
            value = value:match("^%s*(.-)%s*$")
            data[field] = value
        end

        -- 2. required
        if rules.required then
            if value == nil or value == "" then
                err = custom_msg or "is required"
            end
        else
            -- Optional field: if nil, skip remaining rules
            if value == nil then
                goto continue
            end
        end

        if err then goto set_error end

        -- 3. type check
        if rules.type then
            local rt = rules.type
            if rt == "string" then
                if type(value) ~= "string" then
                    err = custom_msg or "must be a string"
                end
            elseif rt == "number" then
                if type(value) ~= "number" then
                    err = custom_msg or "must be a number"
                end
            elseif rt == "integer" then
                if type(value) ~= "number" or value ~= math.floor(value) then
                    err = custom_msg or "must be an integer"
                end
            elseif rt == "boolean" then
                if type(value) ~= "boolean" then
                    err = custom_msg or "must be a boolean"
                end
            end
        end

        if err then goto set_error end

        -- 4. min
        if rules.min then
            if type(value) == "string" then
                if #value < rules.min then
                    err = custom_msg or "must be at least " .. rules.min .. " characters"
                end
            elseif type(value) == "number" then
                if value < rules.min then
                    err = custom_msg or "must be at least " .. rules.min
                end
            else
                -- min/max only bound string length or numeric value; a boolean
                -- or table would silently pass otherwise. Fail closed.
                err = custom_msg or "must be a string or number"
            end
        end

        if err then goto set_error end

        -- 5. max
        if rules.max then
            if type(value) == "string" then
                if #value > rules.max then
                    err = custom_msg or "must be at most " .. rules.max .. " characters"
                end
            elseif type(value) == "number" then
                if value > rules.max then
                    err = custom_msg or "must be at most " .. rules.max
                end
            else
                err = custom_msg or "must be a string or number"
            end
        end

        if err then goto set_error end

        -- 6. pattern
        if rules.pattern then
            if type(value) ~= "string" or not value:match(rules.pattern) then
                err = custom_msg or "does not match the required pattern"
            end
        end

        if err then goto set_error end

        -- 7. oneof
        if rules.oneof then
            local found = false
            for _, v in ipairs(rules.oneof) do
                if value == v then found = true; break end
            end
            if not found then
                err = custom_msg or "must be one of: " .. table.concat(rules.oneof, ", ")
            end
        end

        if err then goto set_error end

        -- 8. email
        if rules.email then
            if not email_ok(value) then
                err = custom_msg or "is not a valid email"
            end
        end

        if err then goto set_error end

        -- 9. fn (custom validator)
        if rules.fn then
            local fn_err = rules.fn(value, field, data)
            if fn_err then
                err = fn_err
            end
        end

        ::set_error::
        if err then
            if not errors then errors = {} end
            errors[field] = err
        end

        ::continue::
    end

    if errors then
        return false, errors
    end
    return true, nil
end

return validate
