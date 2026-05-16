--- RFC 4180 CSV parser + writer.
--
-- @module hull.csv
-- @license AGPL-3.0-or-later
--
-- Character-by-character state machine parser. Correctly handles quoted
-- fields containing the separator, embedded newlines, and doubled-quote
-- escapes (`""`).

local csv = {}

-- States for the parser state machine
local STATE_FIELD_START  = 1
local STATE_UNQUOTED     = 2
local STATE_QUOTED        = 3
local STATE_QUOTE_IN_QUOTED = 4

--- Parse a CSV string.
--
-- @tparam string text  CSV text (UTF-8). `nil` or `""` returns `{}`.
-- @tparam[opt] table opts  Options:
--
--   - `headers`   (boolean, default `false`): first row treated as
--     headers; returns an array of objects (`{ [col_name] = value, ... }`)
--     instead of an array of arrays.
--   - `separator` (string, default `","`): field delimiter.
--   - `quote`     (string, default `'"'`): quote character.
--   - `max_rows`  (integer, default `100_000`): hard cap; rows past this
--     are silently dropped.
--
-- @treturn table[]  Array of row arrays (or row objects if `headers=true`).
-- @usage
-- local rows = csv.parse(req.body, { headers = true })
-- for _, row in ipairs(rows) do print(row.name, row.email) end
function csv.parse(text, opts)
    if text == nil or text == "" then
        return {}
    end

    opts = opts or {}
    local sep   = opts.separator or ","
    local quote = opts.quote or '"'
    local use_headers = opts.headers or false
    local max_rows = opts.max_rows or 100000

    local rows = {}
    local row = {}
    local field = {}
    local state = STATE_FIELD_START

    local len = #text
    local i = 1

    while i <= len do
        if #rows >= max_rows then
            error("csv.parse: exceeded max_rows limit (" .. max_rows .. ")")
        end
        local c = text:sub(i, i)

        if state == STATE_FIELD_START then
            if c == quote then
                -- Begin quoted field
                state = STATE_QUOTED
                i = i + 1
            elseif c == sep then
                -- Empty unquoted field, commit it
                row[#row + 1] = ""
                -- Stay in FIELD_START for next field
                i = i + 1
            elseif c == "\r" then
                -- End of row (CRLF or bare CR)
                row[#row + 1] = ""
                rows[#rows + 1] = row
                row = {}
                state = STATE_FIELD_START
                if i + 1 <= len and text:sub(i + 1, i + 1) == "\n" then
                    i = i + 2
                else
                    i = i + 1
                end
            elseif c == "\n" then
                -- End of row (bare LF)
                row[#row + 1] = ""
                rows[#rows + 1] = row
                row = {}
                state = STATE_FIELD_START
                i = i + 1
            else
                -- Begin unquoted field
                field[#field + 1] = c
                state = STATE_UNQUOTED
                i = i + 1
            end

        elseif state == STATE_UNQUOTED then
            if c == sep then
                -- End of field
                row[#row + 1] = table.concat(field)
                field = {}
                state = STATE_FIELD_START
                i = i + 1
            elseif c == "\r" then
                -- End of row (CRLF or bare CR)
                row[#row + 1] = table.concat(field)
                field = {}
                rows[#rows + 1] = row
                row = {}
                state = STATE_FIELD_START
                if i + 1 <= len and text:sub(i + 1, i + 1) == "\n" then
                    i = i + 2
                else
                    i = i + 1
                end
            elseif c == "\n" then
                -- End of row (bare LF)
                row[#row + 1] = table.concat(field)
                field = {}
                rows[#rows + 1] = row
                row = {}
                state = STATE_FIELD_START
                i = i + 1
            else
                field[#field + 1] = c
                i = i + 1
            end

        elseif state == STATE_QUOTED then
            if c == quote then
                -- Could be end of quoted field or escaped quote
                state = STATE_QUOTE_IN_QUOTED
                i = i + 1
            else
                -- Accumulate character (including newlines, separators, etc.)
                field[#field + 1] = c
                i = i + 1
            end

        elseif state == STATE_QUOTE_IN_QUOTED then
            if c == quote then
                -- Escaped quote (doubled)
                field[#field + 1] = quote
                state = STATE_QUOTED
                i = i + 1
            elseif c == sep then
                -- End of quoted field, then separator
                row[#row + 1] = table.concat(field)
                field = {}
                state = STATE_FIELD_START
                i = i + 1
            elseif c == "\r" then
                -- End of quoted field, then row end
                row[#row + 1] = table.concat(field)
                field = {}
                rows[#rows + 1] = row
                row = {}
                state = STATE_FIELD_START
                if i + 1 <= len and text:sub(i + 1, i + 1) == "\n" then
                    i = i + 2
                else
                    i = i + 1
                end
            elseif c == "\n" then
                -- End of quoted field, then row end
                row[#row + 1] = table.concat(field)
                field = {}
                rows[#rows + 1] = row
                row = {}
                state = STATE_FIELD_START
                i = i + 1
            else
                -- Per RFC 4180 this is malformed, but be lenient:
                -- treat the closing quote as the end of quoting and
                -- continue accumulating as unquoted content
                field[#field + 1] = c
                state = STATE_UNQUOTED
                i = i + 1
            end
        end
    end

    -- Flush the last field/row if there is pending data
    if state == STATE_FIELD_START then
        -- We are at field start — only emit a row if we have accumulated
        -- fields (handles trailing separator case)
        if #row > 0 then
            row[#row + 1] = ""
            rows[#rows + 1] = row
        end
    elseif state == STATE_UNQUOTED then
        row[#row + 1] = table.concat(field)
        rows[#rows + 1] = row
    elseif state == STATE_QUOTED then
        -- Unterminated quoted field — flush what we have
        row[#row + 1] = table.concat(field)
        rows[#rows + 1] = row
    elseif state == STATE_QUOTE_IN_QUOTED then
        -- Closing quote was the last character
        row[#row + 1] = table.concat(field)
        rows[#rows + 1] = row
    end

    -- If headers mode, convert row arrays to objects
    if use_headers and #rows > 0 then
        local header_row = rows[1]
        local result = {}
        for r = 2, #rows do
            local obj = {}
            for c = 1, #header_row do
                obj[header_row[c]] = rows[r][c] or ""
            end
            result[#result + 1] = obj
        end
        return result
    end

    return rows
end

--- Encode rows into a CSV string.
--
-- @tparam table[] rows  Array of rows. When `opts.headers = true`, rows
--   are objects (keys become the header row). Otherwise rows are arrays.
-- @tparam[opt] table opts
--
--   - `headers`   (boolean, default `false`)
--   - `separator` (string, default `","`)
--   - `quote`     (string, default `'"'`)
--
-- @treturn string  CSV text with LF line endings. Values containing the
--   separator, quote, CR, or LF are auto-quoted; embedded quotes are
--   doubled (RFC 4180).
function csv.encode(rows, opts)
    if rows == nil or #rows == 0 then
        return ""
    end

    opts = opts or {}
    local sep   = opts.separator or ","
    local quote = opts.quote or '"'
    local use_headers = opts.headers or false
    local escaped_quote = quote .. quote

    -- Determine if a field value needs quoting
    local function needs_quoting(val)
        if val:find(sep, 1, true) then return true end
        if val:find(quote, 1, true) then return true end
        if val:find("\n", 1, true) then return true end
        if val:find("\r", 1, true) then return true end
        return false
    end

    -- Encode a single field
    local function encode_field(val)
        if val == nil then
            return ""
        end
        val = tostring(val)
        if needs_quoting(val) then
            return quote .. val:gsub(quote, escaped_quote) .. quote
        end
        return val
    end

    -- Encode a single row from an array of values
    local function encode_row(values)
        local fields = {}
        for i = 1, #values do
            fields[i] = encode_field(values[i])
        end
        return table.concat(fields, sep)
    end

    local lines = {}

    if use_headers then
        -- Collect header keys from the first row to establish column order
        local keys = {}
        if opts.columns then
            -- Allow explicit column ordering
            for i = 1, #opts.columns do
                keys[i] = opts.columns[i]
            end
        else
            -- Collect keys from all rows for consistent output
            local key_set = {}
            local key_order = {}
            for _, row in ipairs(rows) do
                for k, _ in pairs(row) do
                    if not key_set[k] then
                        key_set[k] = true
                        key_order[#key_order + 1] = k
                    end
                end
            end
            -- Sort for deterministic output
            table.sort(key_order)
            keys = key_order
        end

        -- Header row
        lines[#lines + 1] = encode_row(keys)

        -- Data rows
        for _, row in ipairs(rows) do
            local values = {}
            for i = 1, #keys do
                values[i] = row[keys[i]]
            end
            lines[#lines + 1] = encode_row(values)
        end
    else
        -- Array of arrays
        for _, row in ipairs(rows) do
            lines[#lines + 1] = encode_row(row)
        end
    end

    return table.concat(lines, "\n") .. "\n"
end

return csv
