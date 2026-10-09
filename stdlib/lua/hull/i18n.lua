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

local _text = require("hull._text")   -- linear trims (see hull._text)

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
-- A range with q <= 0 ("not acceptable", or an unparseable q) is dropped. The
-- sort is by q descending, then by position in the header: table.sort is not
-- stable, so equal-q entries came back in any order (and differently from JS).
local function parse_accept_language(header)
    if not header or header == "" then return {} end
    local entries = {}
    for part in header:gmatch("[^,]+") do
        part = _text.trim(part)
        -- "_" too: "zh_TW" is a common spelling of "zh-TW" (audit 11).
        local lang, rest = part:match("^([%w%-_]+)(.*)")
        if lang then
            local q = 1.0
            local qval = rest:match(";%s*q%s*=%s*([%d%.]+)")
            if qval then q = tonumber(qval) or 0 end
            if q > 0 then
                entries[#entries + 1] = {lang = lang, q = q, i = #entries}
            end
        end
    end
    table.sort(entries, function(a, b)
        if a.q ~= b.q then return a.q > b.q end
        return a.i < b.i
    end)
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
-- The active locale is PROCESS-GLOBAL: every request shares it. Set it
-- and translate within one uninterrupted stretch of code only - a
-- handler that sets it and then yields (db.async, http.fetch, a timer)
-- can resume to find another request's locale. Concurrent requests
-- should pass the locale explicitly with @{i18n.t_in}.
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
    return i18n.t_in(active, key, params)
end

--- Translate a key in an EXPLICIT locale - stateless, so safe across a
-- yield: `i18n.t_in(req.locale, "greeting", {name = n})`. Same lookup,
-- interpolation and fallback (the key itself) as @{i18n.t}.
--
-- @tparam ?string locale  Locale name (`nil` or unknown -> the key).
-- @tparam string key      Translation key.
-- @tparam[opt] table params  Map of `${var}` substitutions.
-- @treturn string
function i18n.t_in(locale, key, params)
    if not locale or not locales[locale] then return key end
    local val = deep_get(locales[locale], key)
    if type(val) ~= "string" then return key end
    return interpolate(val, params)
end

-- The format table of locale @p loc (nil -> none: the built-in defaults).
local function format_of(loc)
    return loc and locales[loc] and locales[loc].format
end

-- NaN or +-inf: printed as Lua prints them, never grouped or rounded.
local function non_finite(n)
    return n ~= n or n == math.huge or n == -math.huge
end

local function number_for(loc, n)
    if type(n) ~= "number" then return tostring(n) end
    if non_finite(n) then return tostring(n) end

    local fmt = format_of(loc)
    local dec_sep = fmt and (fmt.decimalSep or fmt.decimal_sep) or "."
    local thou_sep = fmt and (fmt.thousandsSep or fmt.thousands_sep) or ","

    -- Negate as a float: integer negation wraps math.mininteger to itself.
    local negative = n < 0
    if negative then n = -(n + 0.0) end

    -- Past 1e21 a number prints in exponent form; grouping that gave "1e,+21".
    if n >= 1e21 then return (negative and "-" or "") .. tostring(n) end

    -- Ten fixed decimals, trailing zeros trimmed. The fraction used to be
    -- printed apart ("%.10g"): a tiny one leaked an exponent
    -- (2.00000000001 -> "2.000000083e-11"-style output) and one that rounded
    -- to 1 was dropped instead of carried (1.99999999999 -> "1").
    local int_s, frac_s = string.format("%.10f", n):match("^(%d+)%.(%d+)$")
    frac_s = frac_s:gsub("0+$", "")

    local result = format_int(int_s, thou_sep)
    if frac_s ~= "" then result = result .. dec_sep .. frac_s end

    if negative and result ~= "0" then result = "-" .. result end
    return result
end

--- Format a number using the active locale's separators.
--
-- @tparam number n
-- @treturn string  Formatted; e.g. `1_234_567.89` → `"1,234,567.89"` in en-US.
function i18n.number(n) return number_for(active, n) end

--- Stateless @{i18n.number}: format with locale @p locale's separators
-- (nil or unknown -> the defaults), whatever the process-wide active locale
-- is. Use it in a handler that yields, as @{i18n.t_in}.
function i18n.number_in(locale, n) return number_for(locale, n) end

-- The timestamps date() formats: years 0000 through 9999. Outside it the
-- value is returned as text, like a non-finite one - a timestamp past 2^63
-- reached %04d as a float with no integer form and raised.
local DATE_MIN = -62167219200   -- 0000-01-01T00:00:00Z
local DATE_MAX = 253402300799   -- 9999-12-31T23:59:59Z

local function date_for(loc, timestamp)
    if type(timestamp) ~= "number" or non_finite(timestamp)
       or timestamp < DATE_MIN or timestamp >= DATE_MAX + 1 then
        return tostring(timestamp)
    end

    local fmt = format_of(loc)
    local pattern = fmt and (fmt.datePattern or fmt.date_pattern) or "YYYY-MM-DD"
    if type(pattern) ~= "string" then pattern = "YYYY-MM-DD" end

    -- Every occurrence of a token is replaced (as the JS side does).
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

--- Format a Unix timestamp using the locale's `date_pattern`.
--
-- @tparam integer timestamp  Seconds since epoch.
-- @treturn string  Formatted; supports `YYYY`/`MM`/`DD`/`HH`/`mm`/`ss`
--   tokens in the pattern.
function i18n.date(timestamp) return date_for(active, timestamp) end

--- Stateless @{i18n.date}: locale @p locale's pattern.
function i18n.date_in(locale, timestamp) return date_for(locale, timestamp) end

local function currency_for(loc, amount, code)
    if type(amount) ~= "number" or type(code) ~= "string" then
        return tostring(amount)
    end
    -- %d on a value with no integer form raised ("no integer
    -- representation"): NaN, an infinity, or more minor units than 2^53.
    if non_finite(amount) then return tostring(amount) .. " " .. code end

    local fmt = format_of(loc)
    local cur = fmt and fmt.currency and fmt.currency[code]

    if not cur then
        -- Fallback: formatted number + code
        return number_for(loc, amount) .. " " .. code
    end

    local digits = cur.decimal_digits or cur.decimalDigits or 2

    -- Format the number part
    local dec_sep = fmt.decimalSep or fmt.decimal_sep or "."
    local thou_sep = fmt.thousandsSep or fmt.thousands_sep or ","

    -- The sign is taken off FIRST and the magnitude rounded half away
    -- from zero in whole minor units: floor() ran on the signed value, so
    -- -1.5 became -2 + 0.50 and rendered "-2.50". Integer minor units
    -- also keep the fraction exact (no float remainder to re-round).
    local neg = amount < 0
    local scale = math.tointeger(10 ^ digits) or 1
    if math.abs(amount) * scale >= 2^53 then
        return number_for(loc, amount) .. " " .. code
    end
    local units = math.floor(math.abs(amount) * scale + 0.5)
    local int_part = units // scale
    local frac_part = units % scale
    if units == 0 then neg = false end   -- no "-0.00"

    local result = format_int(string.format("%d", int_part), thou_sep)

    if digits > 0 then
        result = result .. dec_sep .. string.format("%0" .. digits .. "d", frac_part)
    end

    if neg then result = "-" .. result end

    local symbol = cur.symbol or code
    if cur.position == "after" then
        return result .. " " .. symbol
    else
        return symbol .. result
    end
end

--- Format an amount in a given currency.
--
-- @tparam number amount  Numeric value.
-- @tparam string code    ISO 4217 code (e.g. `"USD"`, `"EUR"`, `"HUF"`).
-- @treturn string  Formatted per the active locale's `currency` table.
function i18n.currency(amount, code) return currency_for(active, amount, code) end

--- Stateless @{i18n.currency}: locale @p locale's currency table.
function i18n.currency_in(locale, amount, code)
    return currency_for(locale, amount, code)
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
        local base = entry.lang:match("^([^%-_]+)")
        if base and locales[base] then return base end
    end
    -- Try base language match for all entries (second pass)
    -- Match "en" to "en-GB" but not "end" or "encyclopedia". The loaded names
    -- are walked in sorted order: pairs() order is unspecified, so with both
    -- "en-GB" and "en-US" loaded the pick varied (and differed from JS).
    local names = {}
    for name in pairs(locales) do names[#names + 1] = name end
    table.sort(names)
    for _, entry in ipairs(entries) do
        local base = entry.lang:match("^([^%-_]+)")
        if base then
            for _, name in ipairs(names) do
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
