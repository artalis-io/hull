-- hull.sse-client - read a Server-Sent Events stream
--
-- The event-stream parser of the HTML standard ("9.2 Server-sent events"),
-- over a response from http.async.stream:
--
--   local http = require("hull.http-client")
--   local sse  = require("hull.sse-client")
--   local res, err, code = http.async.stream("GET", "https://api.example.com/feed",
--       { headers = { accept = "text/event-stream" } })
--   if not res then error(err) end
--   for ev in sse.events(res) do
--       -- ev.event ("message" unless the server named one), ev.data,
--       -- ev.id (the last event id seen), ev.retry (the reconnection time
--       -- in ms the server last set, or nil)
--   end
--
-- events(res, opts) returns an iterator: the next event, nil once the
-- stream ended. The stream is closed when the iterator reaches the end or an
-- error, and when a generic `for` is left early (break, an error, a return):
-- the fourth value events() returns is a to-be-closed closer. A stream that
-- ends with an error (the server went quiet past the idle timeout, the
-- reader fell behind, the connection broke) raises a coded error table:
-- err.code is the read's code ("idle", "overflow", "too_large", "closed",
-- "io_error", ...), err.message the read's message, tostring(err)
-- "sse-client: <message> (<code>)". A runtime refusal the read raises
-- ("[hull:busy] ...", "[hull:wait_refused] ...") passes through unchanged.
-- An event the stream ended in the middle of is dropped, as the standard
-- says.
--
-- One event is capped: its pending line plus the data it gathered so far
-- may not pass opts.max_event_bytes (default 1 MiB); past it the parser
-- raises a coded error with code "too_large" (events() closes the stream
-- first). A parser that raised it is spent: every later feed raises again.
--
-- sse.parser(opts) is the parser alone: p:feed(text) returns the events the
-- text completes (a list), p:finish() ends the input. Lines end at CR LF, LF
-- or CR (a CR ends its line at once; an LF right after it, in the next chunk
-- too, is skipped); a leading byte-order mark is skipped, also when it
-- arrives split across chunks; comment lines (":") are ignored; `data` lines
-- join with LF; an `id` holding a NUL is ignored; `retry` takes digits only
-- (a value past 2^53 - 1 is ignored) and stays the reconnection time until
-- another one arrives. A line split over many chunks is scanned once, and
-- its pieces are kept as a tower of O(log n) strings (O(n log n) copying).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local M = {}

local DEFAULT_MAX_EVENT_BYTES = 1048576
local MAX_SAFE_INTEGER = 9007199254740991
local BOM = "\239\187\191"

-- Coded error: err.code is the stable, branch-on identity. Level 0 so the
-- Lua source position is not prepended to the message.
local _err_mt = {
    __tostring = function(e) return "sse-client: " .. e.message .. " (" .. e.code .. ")" end,
}
local function coded(code, message)
    return setmetatable({ code = code, message = message }, _err_mt)
end

local function max_event_bytes(opts, level)
    if opts == nil then return DEFAULT_MAX_EVENT_BYTES end
    if type(opts) ~= "table" then
        error("sse-client: opts must be a table", level + 1)
    end
    local n = opts.max_event_bytes
    if n == nil then return DEFAULT_MAX_EVENT_BYTES end
    if math.type(n) ~= "integer" or n < 1 then
        error("sse-client: max_event_bytes must be a positive integer", level + 1)
    end
    return n
end

local function new_parser(limit)
    local p = {
        started = false,
        head = "",          -- leading bytes held back until the BOM check
        pending = {},       -- pieces of the line not yet ended (a tower)
        pending_len = 0,
        skip_lf = false,    -- the last chunk ended a line at a CR
        data = nil,         -- list of data lines, nil = no data field yet
        data_len = 0,
        event = nil,
        last_id = nil,
        retry = nil,        -- the reconnection time, kept across events
        out = nil,
        failed = nil,
    }

    local function too_large()
        p.failed = coded("too_large", "an event passed max_event_bytes ("
            .. limit .. ")")
        error(p.failed, 0)
    end

    local function dispatch()
        if p.data then
            local ev = {
                event = (p.event ~= nil and p.event ~= "") and p.event or "message",
                data = table.concat(p.data, "\n"),
                id = p.last_id,
                retry = p.retry,
            }
            p.out[#p.out + 1] = ev
        end
        p.data, p.data_len, p.event = nil, 0, nil
    end

    local function field(line)
        if line == "" then dispatch(); return end
        if line:sub(1, 1) == ":" then return end
        local name, value
        local colon = line:find(":", 1, true)
        if colon then
            name = line:sub(1, colon - 1)
            value = line:sub(colon + 1)
            if value:sub(1, 1) == " " then value = value:sub(2) end
        else
            name, value = line, ""
        end
        if name == "data" then
            if p.data then
                p.data_len = p.data_len + 1 + #value
            else
                p.data, p.data_len = {}, #value
            end
            if p.data_len > limit then too_large() end
            p.data[#p.data + 1] = value
        elseif name == "event" then
            p.event = value
        elseif name == "id" then
            if not value:find("\0", 1, true) then p.last_id = value end
        elseif name == "retry" then
            if value:match("^%d+$") then
                local n = tonumber(value)
                if math.type(n) == "integer" and n <= MAX_SAFE_INTEGER then
                    p.retry = n
                end
            end
        end
    end

    -- Scan one chunk once: every line it ends is the pending pieces joined
    -- with the chunk's part, and the rest of the chunk becomes pending.
    local function scan(text)
        local pos, n = 1, #text
        if p.skip_lf and n > 0 then
            if text:sub(1, 1) == "\n" then pos = 2 end
            p.skip_lf = false
        end
        while pos <= n do
            local s = text:find("[\r\n]", pos)
            if not s then break end
            if p.pending_len + (s - pos) + p.data_len > limit then too_large() end
            local line = text:sub(pos, s - 1)
            if p.pending_len > 0 then
                p.pending[#p.pending + 1] = line
                line = table.concat(p.pending)
                p.pending, p.pending_len = {}, 0
            end
            field(line)
            if text:sub(s, s) == "\r" then
                if s == n then
                    p.skip_lf = true
                elseif text:sub(s + 1, s + 1) == "\n" then
                    s = s + 1
                end
            end
            pos = s + 1
        end
        if pos <= n then
            p.pending_len = p.pending_len + (n - pos + 1)
            if p.pending_len + p.data_len > limit then too_large() end
            -- A tower of pieces (PIL 11.6): a piece not shorter than the one
            -- below merges into it, so the list holds O(log n) strings even
            -- when the line arrives a byte at a time, and the total copying
            -- stays O(n log n).
            local pend = p.pending
            pend[#pend + 1] = text:sub(pos)
            while #pend >= 2 and #pend[#pend] >= #pend[#pend - 1] do
                pend[#pend - 1] = pend[#pend - 1] .. pend[#pend]
                pend[#pend] = nil
            end
        end
    end

    function p:feed(text)
        if type(text) ~= "string" then
            error("sse-client: feed takes a string", 2)
        end
        if self.failed then error(self.failed, 0) end
        if not self.started then
            -- Hold up to 3 leading bytes back until the BOM check can be
            -- decided: a BOM may arrive split across chunks.
            text = self.head .. text
            if #text < 3 and BOM:sub(1, #text) == text then
                self.head = text
                return {}
            end
            self.head, self.started = "", true
            if text:sub(1, 3) == BOM then text = text:sub(4) end
        end
        self.out = {}
        scan(text)
        local out = self.out
        self.out = nil
        return out
    end

    function p:finish()
        if self.failed then error(self.failed, 0) end
        -- Whatever is pending is an event the input ended inside: dropped.
        -- (A line ends at its CR at once, so nothing is left to complete.)
        self.head, self.pending, self.pending_len, self.skip_lf = "", {}, 0, false
        self.data, self.data_len, self.event = nil, 0, nil
        return {}
    end

    return p
end

function M.parser(opts)
    return new_parser(max_event_bytes(opts, 2))
end

function M.events(res, opts)
    if type(res) ~= "table" or type(res.read) ~= "function" then
        error("sse-client: events takes a response from http.async.stream", 2)
    end
    local p = new_parser(max_event_bytes(opts, 2))
    local queue, qi, done, closed = {}, 1, false, false
    local function close()
        if closed then return end
        closed = true
        if type(res.close) == "function" then res.close() end
    end
    -- Stop on an error: close the stream without letting a failing close
    -- replace the error being raised.
    local function fail(e)
        done, queue, qi = true, {}, 1
        if not closed then
            closed = true
            if type(res.close) == "function" then pcall(res.close) end
        end
        error(e, 0)
    end
    local closer = setmetatable({}, { __close = function() close() end })
    local function step()
        while true do
            if qi <= #queue then
                local ev = queue[qi]
                queue[qi] = nil
                qi = qi + 1
                return ev
            end
            if done then return nil end
            local chunk, err, code = res.read()
            if chunk then
                local ok, evs = pcall(p.feed, p, chunk)
                if not ok then fail(evs) end
                queue, qi = evs, 1
            elseif err then
                fail(coded(tostring(code), tostring(err)))
            else
                queue, qi = p:finish(), 1
                done = true
                close()
            end
        end
    end
    return step, nil, nil, closer
end

return M
