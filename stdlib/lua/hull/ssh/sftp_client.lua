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

-- A caller's mistake (a bad mode, a negative offset), raised with a code so
-- the facade hands it back as { code = "bad_argument" } like every other
-- failure, rather than as a connection error.
local function bad_argument(detail)
    error({ code = "bad_argument", detail = detail }, 0)
end

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
    return self:collect({ self:submit(build, ...) })[1]
end

-- Send one request without waiting; returns its id. See collect.
function Sftp:submit(build, ...)
    self.id = self.id + 1
    self:send(build(self.id, ...))
    return self.id
end

-- The replies to `ids`, in the order of `ids`.
--
-- Pipelining - several requests in flight before the first answer - is what
-- makes a large transfer fast over a relay: one 32 KiB read per round trip is
-- a crawl at 50 ms. The protocol lets a server answer out of order, so
-- replies are matched by id, never by position, and one for an id nobody is
-- waiting on is a broken session and raises.
function Sftp:collect(ids)
    local want, got, left = {}, {}, #ids
    for _, id in ipairs(ids) do want[id] = true end
    while left > 0 do
        local r = self:recv()
        if not want[r.id] or got[r.id] then
            error("ssh.sftp: reply for request " .. tostring(r.id)
                  .. " while waiting for " .. table.concat(ids, ","))
        end
        got[r.id] = r
        left = left - 1
    end
    local out = {}
    for i, id in ipairs(ids) do out[i] = got[id] end
    return out
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

-- One status reply as a result: true, or nil plus the coded error.
local function status_result(r)
    if r.type == "status" and r.ok then return true end
    return nil, sftp_error(r)
end

-- Attributes as a caller sees them, from a decoded ATTRS reply.
local function attrs_result(r)
    if r.type ~= "attrs" then return nil, sftp_error(r) end
    local a = r.attrs
    return { size = a.size, uid = a.uid, gid = a.gid,
             permissions = a.permissions, atime = a.atime, mtime = a.mtime,
             is_dir = a.is_dir }
end

-- A permission mode as a caller writes it: 0x1ed or 493 for 0755, or the
-- octal string "755". Lua has no octal literal, which is how a caller ends
-- up setting mode 755 decimal (01363) by accident.
local function mode_arg(mode)
    if type(mode) == "string" and mode:match("^[0-7][0-7][0-7][0-7]?$") then
        return tonumber(mode, 8)
    end
    if math.type(mode) == "integer" and mode >= 0 and mode <= 0xFFF then
        return mode
    end
    bad_argument("ssh.sftp: mode must be an octal string such as \"755\", or an integer")
end

--- Resolve a path on the server. Returns the canonical path.
function Sftp:realpath(path)
    local r = self:request(sftp.build_realpath, path)
    if r.type == "status" then return nil, sftp_error(r) end
    return r.names[1] and r.names[1].filename
end

--- A path's attributes, following a symlink: { size, uid, gid, permissions,
--- atime, mtime, is_dir }. Fields the server did not send are nil.
function Sftp:stat(path)
    return attrs_result(self:request(sftp.build_stat, path))
end

--- As stat, but of a symlink itself rather than what it points at.
function Sftp:lstat(path)
    return attrs_result(self:request(sftp.build_lstat, path))
end

--- Create a directory, with `mode` if given ("755").
function Sftp:mkdir(path, mode)
    local attrs = mode ~= nil and { permissions = mode_arg(mode) } or nil
    return status_result(self:request(sftp.build_mkdir, path, attrs))
end

--- Remove an empty directory.
function Sftp:rmdir(path)
    return status_result(self:request(sftp.build_rmdir, path))
end

--- Remove a file.
function Sftp:remove(path)
    return status_result(self:request(sftp.build_remove, path))
end

--- Rename a file or directory. SFTP version 3 refuses when `to` exists
--- (`failure`); remove it first if replacing is what is meant.
function Sftp:rename(from, to)
    return status_result(self:request(sftp.build_rename, from, to))
end

--- Set a path's permission bits ("755").
function Sftp:chmod(path, mode)
    return self:setstat(path, { permissions = mode_arg(mode) })
end

--- Set any of { size, uid, gid, permissions, atime, mtime } on a path.
--- uid and gid go together, as do atime and mtime; the protocol has no way
--- to set one of a pair.
function Sftp:setstat(path, attrs)
    if type(attrs) ~= "table" then
        bad_argument("ssh.sftp: setstat takes an attributes table")
    end
    return status_result(self:request(sftp.build_setstat, path, attrs))
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

-- Files -----------------------------------------------------------------------

-- One read or write request. 32 KiB is what every server honours in full
-- (OpenSSH caps a read at 256 KiB); PIPELINE of them are in flight at once.
local IO_CHUNK = 32768
local PIPELINE = 8

local OPEN_MODES = {
    r    = sftp.FXF_READ,
    ["r+"] = sftp.FXF_READ | sftp.FXF_WRITE,
    w    = sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_TRUNC,
    a    = sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_APPEND,
    wx   = sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_EXCL,
}

local File = {}
File.__index = File

--- Open a file. `mode` is "r" (read), "r+" (read and write, must exist),
--- "w" (create or truncate), "a" (append, creating) or "wx" (create; fails
--- if it exists). `opts.mode` sets the permissions of a file this creates.
---
--- Close it: an open handle holds a file open on the server until it is
--- closed, or until the sftp session closes.
function Sftp:open(path, mode, opts)
    local pflags = OPEN_MODES[mode or "r"]
    if not pflags then
        bad_argument("ssh.sftp: open mode must be r, r+, w, a or wx")
    end
    local attrs = opts and opts.mode ~= nil
                  and { permissions = mode_arg(opts.mode) } or nil
    local h = self:request(sftp.build_open, path, pflags, attrs)
    if h.type ~= "handle" then return nil, sftp_error(h) end
    return setmetatable({ s = self, handle = h.handle, pos = 0,
                          append = mode == "a" }, File)
end

local function closed_error()
    return { code = "closed", detail = "the file is closed" }
end

--- Read up to `n` bytes (default 32 KiB) from the current position. Returns
--- them, "" at end of file, or nil plus a reason. Fewer than `n` bytes means
--- end of file or a server that sent less; the next read continues.
function File:read(n)
    if not self.handle then return nil, closed_error() end
    n = n or IO_CHUNK
    if math.type(n) ~= "integer" or n < 1 then
        bad_argument("ssh.sftp: read size must be a positive integer")
    end
    local parts, got = {}, 0
    while got < n do
        -- One batch of requests for consecutive chunks. The batch starts at
        -- one and doubles each time a batch comes back full, up to PIPELINE:
        -- a small file costs what it always did (one read, then EOF), and a
        -- large one reaches full pipelining within a few round trips.
        local batch = self.ramp or 1
        local ids, sizes = {}, {}
        local off = self.pos + got
        while #ids < batch and got + (#ids * IO_CHUNK) < n do
            local want = math.min(IO_CHUNK, n - got - #ids * IO_CHUNK)
            ids[#ids + 1] = self.s:submit(sftp.build_read, self.handle,
                                          off + #ids * IO_CHUNK, want)
            sizes[#sizes + 1] = want
        end
        local replies = self.s:collect(ids)
        -- Taken in offset order, stopping at the first short or EOF reply:
        -- anything after a gap describes bytes past it, and is asked for
        -- again by the next read rather than returned out of place.
        local short = false
        for i, r in ipairs(replies) do
            if not short then
                if r.type == "data" then
                    parts[#parts + 1] = r.data
                    got = got + #r.data
                    if #r.data < sizes[i] then short = true end
                elseif r.type == "status" and r.eof then
                    short = true
                else
                    self.pos = self.pos + got
                    return nil, sftp_error(r)
                end
            end
        end
        if short then break end
        self.ramp = math.min(PIPELINE, batch * 2)
    end
    self.pos = self.pos + got
    return table.concat(parts)
end

--- Write all of `data` at the current position (at the end, in "a" mode).
--- Returns true, or nil plus a reason.
function File:write(data)
    if not self.handle then return nil, closed_error() end
    if type(data) ~= "string" then
        bad_argument("ssh.sftp: write takes a string")
    end
    local off = 0
    while off < #data do
        local ids = {}
        while #ids < PIPELINE and off < #data do
            local chunk = data:sub(off + 1, off + IO_CHUNK)
            ids[#ids + 1] = self.s:submit(sftp.build_write, self.handle,
                                          self.pos + off, chunk)
            off = off + #chunk
        end
        for _, r in ipairs(self.s:collect(ids)) do
            if not (r.type == "status" and r.ok) then return nil, sftp_error(r) end
        end
    end
    self.pos = self.pos + #data
    return true
end

--- Move the position to `offset` bytes from the start.
function File:seek(offset)
    if math.type(offset) ~= "integer" or offset < 0 then
        bad_argument("ssh.sftp: seek takes a non-negative integer offset")
    end
    self.pos = offset
    return true
end

--- The current position.
function File:tell() return self.pos end

--- The open file's attributes, as Sftp:stat.
function File:stat()
    if not self.handle then return nil, closed_error() end
    return attrs_result(self.s:request(sftp.build_fstat, self.handle))
end

--- Close the file. Returns true, or nil plus the server's reason - a write
--- the server could only fail at close is reported here. Closing twice is
--- harmless.
function File:close()
    if not self.handle then return true end
    local h = self.handle
    self.handle = nil
    return status_result(self.s:close_handle(h))
end

-- Whole files, on top of the above ---------------------------------------------

--- Read a whole file. `max` (default 16 MiB) bounds it, because the server
--- chooses the size.
function Sftp:read(path, max)
    max = max or (16 * 1024 * 1024)
    local f, err = self:open(path, "r")
    if not f then return nil, err end
    local parts, total = {}, 0
    while true do
        local chunk, rerr = f:read(PIPELINE * IO_CHUNK)
        if not chunk then f:close(); return nil, rerr end
        if chunk == "" then break end
        total = total + #chunk
        if total > max then
            f:close()
            return nil, { code = "too_large", limit = max,
                          detail = "file exceeds the " .. tostring(max) .. " byte limit" }
        end
        parts[#parts + 1] = chunk
    end
    f:close()
    return table.concat(parts)
end

--- Write a whole file, creating or truncating it.
function Sftp:write(path, data)
    local f, err = self:open(path, "w")
    if not f then return nil, err end
    local ok, werr = f:write(data)
    if not ok then f:close(); return nil, werr end
    return f:close()
end

function Sftp:close()
    self.t:close_channel(self.ch)
end

return M
