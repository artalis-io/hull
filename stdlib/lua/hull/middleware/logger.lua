--
-- hull.middleware.logger -- Request logging middleware (logfmt)
--
-- Logs incoming requests as single-line key=value pairs (logfmt).
-- Sets X-Request-ID header for request tracing.
--
-- logger.generate_id()            - hex counter string for request ID
-- logger.format_line(entries)     - key=value pairs -> single string
-- logger.should_skip(path, skip)  - exact-match path check
-- logger.middleware(opts)         - returns middleware function
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local logger = {}

local _counter = 0

--- Generate an incrementing hex request ID.
function logger.generate_id()
    _counter = _counter + 1
    return string.format("%x", _counter)
end

--- Sanitize a value for safe logfmt output (prevent log injection).
local function sanitize_value(v)
    v = v:gsub("\\", "\\\\")
    v = v:gsub("\n", "\\n")
    v = v:gsub("\r", "\\r")
    v = v:gsub('"', '\\"')
    return v
end

--- Format a list of {key, value} pairs into a logfmt line.
-- Values are sanitized and quoted when they contain special characters.
function logger.format_line(entries)
    local parts = {}
    for _, entry in ipairs(entries) do
        local k = entry[1]
        local v = sanitize_value(tostring(entry[2]))
        if v:find("[ =\"\n\r]") then
            parts[#parts + 1] = k .. '="' .. v .. '"'
        else
            parts[#parts + 1] = k .. "=" .. v
        end
    end
    return table.concat(parts, " ")
end

--- Check whether a path is in the skip list (exact match).
function logger.should_skip(path, skip_list)
    if not skip_list then return false end
    for _, p in ipairs(skip_list) do
        if p == path then return true end
    end
    return false
end

--- Create a logging middleware function for use with app.use().
-- opts.skip: list of paths to skip (exact match)
-- opts.include_headers: list of header names to include in log line
function logger.middleware(opts)
    opts = opts or {}
    local skip = opts.skip
    local include_headers = opts.include_headers

    return function(req, res)
        if logger.should_skip(req.path, skip) then
            return 0
        end

        local req_id = logger.generate_id()
        req.ctx.request_id = req_id
        res:header("X-Request-ID", req_id)

        local entries = {
            { "method", req.method },
            { "path", req.path },
            { "req_id", req_id },
            { "body_in", req.content_length or 0 },
        }

        if include_headers then
            for _, name in ipairs(include_headers) do
                local val = req.headers[name:lower()]
                if val then
                    entries[#entries + 1] = { name:lower():gsub("-", "_"), val }
                end
            end
        end

        log.info("req " .. logger.format_line(entries))

        return 0
    end
end

return logger
