-- hull.ssh.sftp_client - an SFTP session over one channel of a connection.
--
-- The protocol codec is hull.ssh.sftp (bytes <-> tables, no I/O). This is the
-- client on top of it: a session on its own channel, requests matched to their
-- replies by id, and the operations an application calls. It moves bytes only
-- through the connection it is given (hull.ssh.transport), so it holds no
-- authority of its own.
--
-- Internal: applications reach it through a connection's sftp() method, which
-- hands out a handle rather than this object.

local channel = require('hull.ssh.channel')
local sftp    = require('hull.ssh.sftp')

local M = {}

local Sftp = {}
Sftp.__index = Sftp

-- A reply that is not the one asked for, as the error a caller branches on:
-- the server's status by name (`no_such_file`, `permission_denied`, ...),
-- its sanitised text as detail, and the raw status number.
local function sftp_error(r)
    if r.type == "status" then
        return { code = r.name, detail = r.text, status = r.code }
    end
    return { code = "bad_reply", detail = "unexpected " .. tostring(r.type) }
end

--- Open an SFTP session on its own channel of connection `t`.
---
--- Paths travel as length-prefixed strings inside the subsystem, never as
--- words in a command line, which is the whole reason file transfer here
--- needs no shell quoting.
function M.open(t)
    local ch, cerr = t:open_session()
    if not ch then return nil, cerr end

    t:send_packet(channel.build_subsystem(ch.remote_id, "sftp"))
    if not t:await_channel_reply(ch) then
        return nil, { code = "sftp_unavailable" }
    end

    local s = setmetatable({ t = t, ch = ch, buf = "", id = 0 }, Sftp)
    s:send(sftp.build_init())
    local ver = s:recv()
    if ver.type ~= "version" then
        return nil, { code = "sftp_no_version" }
    end
    s.version = ver.version
    return s
end

-- Send one SFTP message, split across as many CHANNEL_DATA messages as the
-- peer's window and packet size require.
--
-- SFTP is a byte stream inside the channel, so a message may be cut anywhere.
-- It used to go out whole, and data_message REFUSES rather than truncates:
-- a peer advertising a 16 KiB packet size, or a window not yet topped up when
-- the next write went out, made a write raise part way through a file.
function Sftp:send(payload)
    local bytes, off = sftp.frame(payload), 1
    while off <= #bytes do
        local room = self.ch:sendable()
        while room <= 0 do
            -- Only the peer can grant more, and while we wait it may still be
            -- sending reply bytes: pump keeps them, it does not drop them.
            self:pump()
            room = self.ch:sendable()
        end
        local chunk = bytes:sub(off, off + room - 1)
        self.t:send_packet(self.ch:data_message(chunk))
        off = off + #chunk
    end
end

-- Read one connection message for this session's channel: keep its data for
-- recv, and top up the window we grant.
function Sftp:pump()
    local m = self.t:channel_message(self.ch)
    if m.type == "data" then
        self.buf = self.buf .. m.data
    elseif m.type == "close" then
        error("ssh.sftp: the channel closed mid-request")
    end
    local adj = self.ch:window_adjustment()
    if adj then self.t:send_packet(adj) end
end

-- Send one request and return ITS reply.
--
-- `build` is one of the sftp.build_* functions; the request id is allocated
-- here and passed as its first argument. Every reply carries the id of the
-- request it answers, and this refuses any other. Replies are otherwise
-- matched by position alone, so one reply left unread - a CLOSE sent on an
-- error path and never collected - shifts every later answer by one: an OPEN
-- then receives the previous file's handle, and the data meant for one file
-- is written into another. A mismatch is a broken session, not a recoverable
-- answer, so it raises rather than being handed back as data.
function Sftp:request(build, ...)
    self.id = self.id + 1
    local id = self.id
    self:send(build(id, ...))
    local r = self:recv()
    if r.id ~= id then
        error("ssh.sftp: reply for request " .. tostring(r.id)
              .. " while waiting for " .. tostring(id))
    end
    return r
end

-- Close a handle and collect the server's answer, on every path. Returns the
-- status reply.
function Sftp:close_handle(handle)
    return self:request(sftp.build_close, handle)
end

-- Pull one SFTP message, feeding the channel as data arrives.
function Sftp:recv()
    for _ = 1, 100000 do
        local p, used = sftp.parse_frame(self.buf)
        if p then
            self.buf = self.buf:sub(used + 1)
            return sftp.parse(p)
        end
        self:pump()
    end
    error("ssh.sftp: no response")
end

--- Resolve a path on the server. Returns the canonical path.
function Sftp:realpath(path)
    local r = self:request(sftp.build_realpath, path)
    if r.type == "status" then return nil, sftp_error(r) end
    return r.names[1] and r.names[1].filename
end

--- List a directory. Returns the entries SAFE to use as local names, plus the
--- ones refused and why - refused rather than dropped, because a name
--- rejected for traversal is something an operator should hear about.
function Sftp:list(path)
    local h = self:request(sftp.build_opendir, path)
    if h.type ~= "handle" then return nil, sftp_error(h) end

    local all = {}
    for _ = 1, 4096 do
        local r = self:request(sftp.build_readdir, h.handle)
        if r.type == "status" then
            -- EOF ends the listing; anything else is a real failure.
            if not r.eof then
                self:close_handle(h.handle)
                return nil, sftp_error(r)
            end
            break
        end
        for _, e in ipairs(r.names) do all[#all + 1] = e end
    end
    self:close_handle(h.handle)
    return sftp.safe_names(all)
end

--- Read a whole file. `max` bounds it, because the server chooses the size.
function Sftp:read(path, max)
    max = max or (16 * 1024 * 1024)
    local h = self:request(sftp.build_open, path, sftp.FXF_READ)
    if h.type ~= "handle" then return nil, sftp_error(h) end

    local parts, off, total = {}, 0, 0
    for _ = 1, 100000 do
        local r = self:request(sftp.build_read, h.handle, off, 32768)
        if r.type == "status" then
            if r.eof then break end
            self:close_handle(h.handle)
            return nil, sftp_error(r)
        end
        total = total + #r.data
        if total > max then
            self:close_handle(h.handle)
            return nil, { code = "too_large", limit = max,
                         detail = "file exceeds the " .. tostring(max) .. " byte limit" }
        end
        parts[#parts + 1] = r.data
        off = off + #r.data
    end
    self:close_handle(h.handle)
    return table.concat(parts)
end

--- Write a whole file, creating or truncating it.
function Sftp:write(path, data)
    local h = self:request(sftp.build_open, path,
        sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_TRUNC)
    if h.type ~= "handle" then return nil, sftp_error(h) end

    local off = 0
    while off < #data do
        -- Chunked to stay under the channel packet cap; the channel refuses
        -- an oversized message rather than truncating it.
        local chunk = data:sub(off + 1, off + 16384)
        local r = self:request(sftp.build_write, h.handle, off, chunk)
        if r.type ~= "status" or not r.ok then
            self:close_handle(h.handle)
            return nil, sftp_error(r)
        end
        off = off + #chunk
    end
    local st = self:close_handle(h.handle)
    if st.type == "status" and not st.ok then return nil, sftp_error(st) end
    return true
end

function Sftp:close()
    self.t:close_channel(self.ch)
end

return M
