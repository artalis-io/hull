--
-- hull.validate -- Schema-based data validation
--
-- Pure function for validating tables against a schema definition.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local validate = {}

local EMAIL_PATTERN = "^[^%s@]+@[^%s@]+%.[^%s@]+$"

--- Validate a data table against a schema.
-- Returns: ok (boolean), errors (table or nil)
--
-- Schema example:
--   { email = { required = true, email = true },
--     name  = { required = true, trim = true, max = 100 } }
--
-- Error table maps field name to error message string.
function validate.check(data, schema)
    if type(data) ~= "table" then data = {} end
    if type(schema) ~= "table" then return true, nil end

    local errors = nil

    for field, rules in pairs(schema) do
        local value = data[field]
        local err = nil
        local custom_msg = rules.message

        -- 1. trim (mutates in-place, not an error)
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
            if type(value) ~= "string" or not value:match(EMAIL_PATTERN) then
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
