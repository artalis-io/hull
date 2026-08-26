--- Lightweight internationalization.
--
-- @module hull.i18n
-- @license AGPL-3.0-or-later
--
-- Locale-aware string lookup with `${var}` interpolation, number / date /
-- currency formatting per locale rules, and an `Accept-Language` parser.
--
-- @usage
-- local i18n = require("hull.i18n")
-- i18n.load("en", { hello = "Hello, ${name}!" })
-- i18n.load("hu", { hello = "Szia, ${name}!" })
-- i18n.locale(i18n.detect(req.headers["accept-language"]) or "en")
-- res:html(i18n.t("hello", { name = "Alice" }))

local i18n = {}

-- ── Internal state ──────────────────────────────────────────────────

local locales = {}      -- name -> locale table
local active = nil      -- current locale name

-- ── Helpers ─────────────────────────────────────────────────────────

-- Traverse a nested table by dotted key path.
-- deep_get({a={b="x"}}, "a.b") -> "x"
local function deep_get(tbl, key)
    local node = tbl
    for part in key:gmatch("[^.]+") do
        if type(node) ~= "table" then return nil end
        node = node[part]
    end
    return node
end

-- Replace ${key} placeholders with values from params table.
local function interpolate(str, params)
    if not params then return str end
    return (str:gsub("%${(%w+)}", function(k)
        local v = params[k]
        if v == nil then return "${" .. k .. "}" end
        return tostring(v)
    end))
end

-- Format an integer string with thousands separator.
-- format_int("1500", " ") -> "1 500"
local function format_int(s, sep)
    local len = #s
    if len <= 3 then return s end
    local parts = {}
    local pos = len % 3
    if pos > 0 then parts[#parts + 1] = s:sub(1, pos) end
    for i = pos + 1, len, 3 do
        parts[#parts + 1] = s:sub(i, i + 2)
    end
    return table.concat(parts, sep)
end

-- Pure-arithmetic epoch (seconds) to UTC date components.
-- Returns {year, month, day, hour, min, sec}.
local function epoch_to_utc(ts)
    ts = math.floor(ts)
    local sec = ts % 60; ts = (ts - sec) / 60
    local min = ts % 60; ts = (ts - min) / 60
    local hour = ts % 24; ts = (ts - hour) / 24
    -- ts is now days since 1970-01-01
    -- Civil calendar from day count (Rata Die variant)
    local z = ts + 719468
    local era = math.floor(z / 146097)
    local doe = z - era * 146097                               -- [0, 146096]
    local yoe = math.floor((doe - math.floor(doe/1460) + math.floor(doe/36524) - math.floor(doe/146096)) / 365)
    local y = yoe + era * 400
    local doy = doe - (365*yoe + math.floor(yoe/4) - math.floor(yoe/100))
    local mp = math.floor((5*doy + 2) / 153)
    local d = doy - math.floor((153*mp + 2) / 5) + 1
    local m = mp + (mp < 10 and 3 or -9)
    if m <= 2 then y = y + 1 end
    return {year = y, month = m, day = d, hour = hour, min = min, sec = sec}
end

-- Parse Accept-Language header into sorted list of {lang, q}.
-- "en-US,en;q=0.9,hu;q=0.8" -> {{lang="en-US",q=1},{lang="en",q=0.9},{lang="hu",q=0.8}}
local function parse_accept_language(header)
    if not header or header == "" then return {} end
    local entries = {}
    for part in header:gmatch("[^,]+") do
        part = part:match("^%s*(.-)%s*$")
        local lang, rest = part:match("^([%w%-]+)(.*)")
        if lang then
            local q = 1.0
            local qval = rest:match(";%s*q%s*=%s*([%d%.]+)")
            if qval then q = tonumber(qval) or 0 end
            entries[#entries + 1] = {lang = lang, q = q}
        end
    end
    -- Sort by quality descending (stable: preserve order for equal q)
    table.sort(entries, function(a, b) return a.q > b.q end)
    return entries
end

-- ── Public API ──────────────────────────────────────────────────────

--- Register a translation table under a locale name.
--
-- @tparam string name  Locale code (e.g. `"en"`, `"hu"`, `"pt-BR"`).
-- @tparam table tbl    Map of message keys → strings. May contain
--   `${variable}` placeholders that @{i18n.t} will interpolate.
function i18n.load(name, tbl)
    if type(name) ~= "string" or type(tbl) ~= "table" then
        error("i18n.load: expected (string, table)")
    end
    locales[name] = tbl
end

--- Get or set the active locale.
--
-- @tparam[opt] string name  When non-nil, sets the active locale.
-- @treturn ?string  Current locale name after the call (or `nil` if none).
function i18n.locale(name)
    if name ~= nil then
        active = name
    end
    return active
end

--- Translate a key in the active locale, with `${var}` interpolation.
--
-- Supports dotted paths (`"errors.not_found"`) - nested tables in the
-- registered locale are walked.
--
-- @tparam string key       Translation key.
-- @tparam[opt] table params  Map of `${var}` substitutions.
-- @treturn string  Translated string, or the key itself if not found
--   (standard fallback so missing keys are visible in development).
function i18n.t(key, params)
    if not active or not locales[active] then return key end
    local val = deep_get(locales[active], key)
    if type(val) ~= "string" then return key end
    return interpolate(val, params)
end

--- Format a number using the active locale's separators.
--
-- @tparam number n
-- @treturn string  Formatted; e.g. `1_234_567.89` → `"1,234,567.89"` in en-US.
function i18n.number(n)
    if type(n) ~= "number" then return tostring(n) end

    local fmt = locales[active] and locales[active].format
    local dec_sep = fmt and (fmt.decimalSep or fmt.decimal_sep) or "."
    local thou_sep = fmt and (fmt.thousandsSep or fmt.thousands_sep) or ","

    local negative = n < 0
    if negative then n = -n end

    local int_part = math.floor(n)
    local frac_part = n - int_part

    local result = format_int(tostring(int_part), thou_sep)

    if frac_part > 0 then
        -- Convert fractional part to string, strip leading "0."
        local frac_str = string.format("%.10g", frac_part)
        local dot_pos = frac_str:find(".", 1, true)
        if dot_pos then
            result = result .. dec_sep .. frac_str:sub(dot_pos + 1)
        end
    end

    if negative then result = "-" .. result end
    return result
end

--- Format a Unix timestamp using the locale's `date_pattern`.
--
-- @tparam integer timestamp  Seconds since epoch.
-- @treturn string  Formatted; supports `YYYY`/`MM`/`DD`/`HH`/`mm`/`ss`
--   tokens in the pattern.
function i18n.date(timestamp)
    if type(timestamp) ~= "number" then return tostring(timestamp) end

    local fmt = locales[active] and locales[active].format
    local pattern = fmt and (fmt.datePattern or fmt.date_pattern) or "YYYY-MM-DD"

    local dt = epoch_to_utc(timestamp)
    local result = pattern
    result = result:gsub("YYYY", string.format("%04d", dt.year))
    result = result:gsub("MM",   string.format("%02d", dt.month))
    result = result:gsub("DD",   string.format("%02d", dt.day))
    result = result:gsub("HH",   string.format("%02d", dt.hour))
    result = result:gsub("mm",   string.format("%02d", dt.min))
    result = result:gsub("ss",   string.format("%02d", dt.sec))
    return result
end

--- Format an amount in a given currency.
--
-- @tparam number amount  Numeric value.
-- @tparam string code    ISO 4217 code (e.g. `"USD"`, `"EUR"`, `"HUF"`).
-- @treturn string  Formatted per the active locale's `currency` table.
function i18n.currency(amount, code)
    if type(amount) ~= "number" or type(code) ~= "string" then
        return tostring(amount)
    end

    local fmt = locales[active] and locales[active].format
    local cur = fmt and fmt.currency and fmt.currency[code]

    if not cur then
        -- Fallback: formatted number + code
        return i18n.number(amount) .. " " .. code
    end

    local digits = cur.decimal_digits or 2
    local rounded = math.floor(amount * 10^digits + 0.5) / 10^digits

    -- Format the number part
    local dec_sep = fmt.decimalSep or fmt.decimal_sep or "."
    local thou_sep = fmt.thousandsSep or fmt.thousands_sep or ","

    local int_part = math.floor(rounded)
    local frac_part = rounded - int_part
    local neg = int_part < 0
    if neg then int_part = -int_part end

    local result = format_int(tostring(int_part), thou_sep)

    if digits > 0 then
        local frac_str = string.format("%0" .. digits .. "d",
            math.floor(frac_part * 10^digits + 0.5))
        result = result .. dec_sep .. frac_str
    end

    if neg then result = "-" .. result end

    local symbol = cur.symbol or code
    if cur.position == "after" then
        return result .. " " .. symbol
    else
        return symbol .. result
    end
end

--- Pick the best matching locale from an `Accept-Language` header.
--
-- Parses RFC 7231 quality pairs (`en;q=0.8, hu;q=0.6`) and returns the
-- highest-priority match among the loaded locales.
--
-- @param header_or_req  Either an `Accept-Language` header value string,
--   or a request object (`req`) from which the header is auto-read via
--   `req:header("Accept-Language")`.
-- @treturn ?string  Matched locale name, or `nil` if no overlap.
function i18n.detect(header_or_req)
    local header = header_or_req
    -- Duck-type: if it has a .header method, call it
    if type(header_or_req) == "table" and type(header_or_req.header) == "function" then
        header = header_or_req:header("Accept-Language")
    end
    if type(header) ~= "string" then return nil end

    local entries = parse_accept_language(header)
    for _, entry in ipairs(entries) do
        -- Exact match
        if locales[entry.lang] then return entry.lang end
        -- Base language match: "en-US" matches loaded "en"
        local base = entry.lang:match("^([%w]+)")
        if base and locales[base] then return base end
    end
    -- Try base language match for all entries (second pass)
    -- Match "en" to "en-GB" but not "end" or "encyclopedia"
    for _, entry in ipairs(entries) do
        local base = entry.lang:match("^([%w]+)")
        if base then
            for name, _ in pairs(locales) do
                if name == base or
                   (name:sub(1, #base) == base and
                    (name:sub(#base + 1, #base + 1) == "-" or
                     name:sub(#base + 1, #base + 1) == "_")) then
                    return name
                end
            end
        end
    end
    return nil
end

--- Reset all state (for testing).
function i18n.reset()
    locales = {}
    active = nil
end

return i18n
