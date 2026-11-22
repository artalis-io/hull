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
--       -- ev.id (the last event id seen), ev.retry (ms, when sent)
--   end
--
-- events(res) returns an iterator: the next event, nil once the stream
-- ended. A stream that ends with an error (the server went quiet past the
-- idle timeout, the reader fell behind, the connection broke) raises
-- "sse-client: <message> (<code>)"; a runtime refusal the read raises
-- ("[hull:busy] ...", "[hull:wait_refused] ...") passes through unchanged. An event the stream
-- ended in the middle of is dropped, as the standard says.
--
-- sse.parser() is the parser alone: p:feed(text) returns the events the
-- text completes (a list), p:finish() ends the input and returns the events
-- a trailing CR completed. Lines end at CR LF,
-- LF or CR, a CR at the end of one chunk waiting for the next; a leading
-- byte-order mark is skipped; comment lines (":") are ignored; `data` lines
-- join with LF; an `id` holding a NUL is ignored; `retry` takes digits only.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local M = {}

local function new_parser()
    local p = {
        buf = "",
        started = false,
        data = nil,         -- list of data lines, nil = no data field yet
        event = nil,
        last_id = nil,
        retry = nil,
        out = nil,
    }

    local function dispatch()
        if p.data then
            local ev = {
                event = p.event or "message",
                data = table.concat(p.data, "\n"),
                id = p.last_id,
                retry = p.retry,
            }
            p.out[#p.out + 1] = ev
        end
        p.data, p.event, p.retry = nil, nil, nil
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
            p.data = p.data or {}
            p.data[#p.data + 1] = value
        elseif name == "event" then
            p.event = value
        elseif name == "id" then
            if not value:find("\0", 1, true) then p.last_id = value end
        elseif name == "retry" then
            if value:match("^%d+$") then p.retry = tonumber(value) end
        end
    end

    -- Process every complete line in p.buf; `final` = no more input, so a
    -- trailing CR ends its line.
    local function drain(final)
        local buf, pos, n = p.buf, 1, #p.buf
        while pos <= n do
            local s = buf:find("[\r\n]", pos)
            if not s then break end
            local c = buf:sub(s, s)
            if c == "\r" and s == n and not final then break end
            field(buf:sub(pos, s - 1))
            if c == "\r" and buf:sub(s + 1, s + 1) == "\n" then s = s + 1 end
            pos = s + 1
        end
        p.buf = buf:sub(pos)
    end

    function p:feed(text)
        if type(text) ~= "string" then
            error("sse-client: feed takes a string", 2)
        end
        if not self.started then
            if text == "" then return {} end
            self.started = true
            if text:sub(1, 3) == "\239\187\191" then text = text:sub(4) end
        end
        self.out = {}
        self.buf = self.buf .. text
        drain(false)
        local out = self.out
        self.out = nil
        return out
    end

    function p:finish()
        self.out = {}
        drain(true)   -- a trailing CR ends its line now
        local out = self.out
        -- Whatever is left is an event the input ended inside: dropped.
        self.out, self.buf = nil, ""
        self.data, self.event, self.retry = nil, nil, nil
        return out
    end

    return p
end

function M.parser()
    return new_parser()
end

function M.events(res)
    if type(res) ~= "table" or type(res.read) ~= "function" then
        error("sse-client: events takes a response from http.async.stream", 2)
    end
    local p = new_parser()
    local queue, qi, done = {}, 1, false
    return function()
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
                queue, qi = p:feed(chunk), 1
            elseif err then
                error("sse-client: " .. tostring(err) .. " (" .. tostring(code) .. ")", 0)
            else
                queue, qi = p:finish(), 1
                done = true
            end
        end
    end
end

return M
