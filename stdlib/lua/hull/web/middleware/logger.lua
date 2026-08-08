local log = require("hull.log")
local _logfmt = require("hull.web._logfmt")

--- Request logging middleware (logfmt output).
--
-- @module hull.web.middleware.logger
-- @license AGPL-3.0-or-later
--
-- Emits one logfmt line per request to the configured logger, e.g.
-- `time=… level=info req_id=… method=GET path=/users status=200 dur_ms=12`.
-- Also assigns a monotonic request id (`req.ctx.request_id`) and sets
-- the `X-Request-ID` response header.

local logger = {}

local _counter = 0

--- Generate an incrementing hex request id.
--
-- Process-local counter; not unique across instances. For distributed
-- tracing add an upstream id (e.g. from a load balancer) and pass it
-- through `req.headers["x-request-id"]` instead.
--
-- @treturn string  Hex string. Minimum 8 chars (counter < 2^32);
--                  grows up to 12 chars before the cap at 2^48.
function logger.generate_id()
    _counter = _counter + 1
    if _counter > 0xffffffffffff then _counter = 1 end
    return string.format("%08x", _counter)
end

--- Format `{key, value}` pairs into a logfmt line.
--
-- Escaping + quoting is the shared `hull.web._logfmt` rule (escapes
-- `\ CR LF "`; quotes values containing a space, `=`, `"`, or a CR/LF). The
-- same rule backs `hull.logx`, so the two logfmt producers can't drift.
--
-- @tparam {{string,any},...} entries  List of pairs.
-- @treturn string  Single-line logfmt-encoded.
function logger.format_line(entries)
    local parts = {}
    for _, entry in ipairs(entries) do
        parts[#parts + 1] = _logfmt.pair(entry[1], entry[2])
    end
    return table.concat(parts, " ")
end

--- Check whether a path is in the skip list (exact match).
--- Test whether `path` should be skipped by the logger.
--
-- @tparam string path
-- @tparam[opt] {string,...} skip_list  Exact paths to skip (no glob).
-- @treturn boolean
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
--- Build the logging middleware.
--
-- Pre-body middleware: sets `req.ctx.request_id` and the response
-- `X-Request-ID` header. Logs one line at request end with method,
-- path, status, and duration.
--
-- @tparam[opt] table opts
--
--   - `skip` (`{string,...}`): paths to skip entirely (e.g. `{"/health"}`).
--   - `include_headers` / `includeHeaders` (`{string,...}`): header names
--     to include in the log line as `header_<name>="..."`.
--
-- @treturn function  Middleware `(req, res) -> integer` (always returns 0).
-- @usage
-- app.use("*", "/*", logger.middleware({ skip = {"/health"} }))
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
