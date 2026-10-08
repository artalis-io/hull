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

-- The separator and the quote are each ONE ASCII character, neither CR nor
-- LF, and not the same one. The parser compares one character at a time, so a
-- multi-character separator never matched (one field per line) while encode
-- joined with it; the JS side behaves the same way, so both refuse it.
local function delims(opts, fn)
    local sep   = opts.separator or ","
    local quote = opts.quote or '"'
    for _, d in ipairs({ { "separator", sep }, { "quote", quote } }) do
        local v = d[2]
        if type(v) ~= "string" or #v ~= 1 or v:byte() >= 0x80
           or v == "\r" or v == "\n" then
            error(fn .. ": opts." .. d[1] .. " must be one ASCII character (not CR / LF)")
        end
    end
    if sep == quote then
        error(fn .. ": opts.separator and opts.quote must differ")
    end
    return sep, quote
end

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
--   - `max_rows`  (integer, default `100_000`): hard cap; parsing THROWS
--     (error) if the input exceeds it. A cap breach is a failure, not a
--     truncated success (see docs/stdlib_style.md §1).
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
    local sep, quote = delims(opts, "csv.parse")
    local use_headers = opts.headers or false
    local max_rows = opts.max_rows or 100000

    -- A leading UTF-8 byte-order mark (Excel writes one) is not data: it
    -- became part of the first header name, so row["name"] was nil.
    if text:sub(1, 3) == "\239\187\191" then text = text:sub(4) end

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
        -- We are at field start - only emit a row if we have accumulated
        -- fields (handles trailing separator case)
        if #row > 0 then
            row[#row + 1] = ""
            rows[#rows + 1] = row
        end
    elseif state == STATE_UNQUOTED then
        row[#row + 1] = table.concat(field)
        rows[#rows + 1] = row
    elseif state == STATE_QUOTED then
        -- Unterminated quoted field - flush what we have
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
--   - `sanitize_formulas` (boolean, default `true`) Prefix a `'` to any cell
--     beginning with `= + - @` (or a leading tab/CR) to neutralize spreadsheet
--     formula/DDE injection when the export is opened in Excel/Sheets. A plain
--     number (`-5`, `+3.2`, `1e-3`) is left alone: it cannot carry a formula.
--     Pass `false` for output that is not meant for a spreadsheet.
--
-- @treturn string  CSV text with LF line endings. Values containing the
--   separator, quote, CR, or LF are auto-quoted; embedded quotes are
--   doubled (RFC 4180).
function csv.encode(rows, opts)
    if rows == nil or #rows == 0 then
        return ""
    end

    opts = opts or {}
    local sep, quote = delims(opts, "csv.encode")
    local use_headers = opts.headers or false
    -- CSV formula-injection defense, ON by default: an export usually ends up
    -- in a spreadsheet, and a cell an attacker controls (a name, a comment)
    -- beginning with = + - @ ran as a formula/DDE there. Prefix a "'" so it
    -- is text. See the doc comment above csv.encode.
    local sanitize = opts.sanitize_formulas ~= false
    local escaped_quote = quote .. quote
    local quote_pat = quote:gsub("%p", "%%%0")

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
        if sanitize and #val > 0 then
            local c = val:sub(1, 1)
            if (c == "=" or c == "+" or c == "-" or c == "@"
                or c == "\t" or c == "\r")
               and not (val:match("^[+-]?%d+%.?%d*$")
                        or val:match("^[+-]?%d+%.?%d*[eE][+-]?%d+$")) then
                val = "'" .. val
            end
        end
        if needs_quoting(val) then
            -- The quote doubled as a literal: it was the gsub PATTERN (and
            -- the doubled quote the replacement), so a quote such as "." or
            -- "%" matched every character or raised.
            local doubled = val:gsub(quote_pat, function() return escaped_quote end)
            return quote .. doubled .. quote
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
            -- String and number keys only (another key has no stable text
            -- form), sorted by text and then type, so a mix of the two sorts
            -- deterministically - table.sort raised comparing a number with
            -- a string.
            local key_set = {}
            local key_order = {}
            for _, row in ipairs(rows) do
                for k, _ in pairs(row) do
                    local tk = type(k)
                    if (tk == "string" or tk == "number") and not key_set[k] then
                        key_set[k] = true
                        key_order[#key_order + 1] = k
                    end
                end
            end
            table.sort(key_order, function(a, b)
                local sa, sb = tostring(a), tostring(b)
                if sa ~= sb then return sa < sb end
                return type(a) < type(b)
            end)
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
