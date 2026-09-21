-- hull.ssh.sftp - the SFTP subsystem protocol, version 3.
--
-- draft-ietf-secsh-filexfer-02, which is what OpenSSH speaks. Version 3 and
-- not something later because that is what servers actually implement; a
-- client that insists on 6 talks to almost nothing.
--
-- This module is why file transfer needs no shell quoting. A path travels as
-- a length-prefixed string inside a binary protocol carried on its own
-- channel - it is never concatenated into a command line, so a file called
-- `; rm -rf / ` is just a file with an unusual name. That property comes from
-- using the subsystem at all; nothing here has to defend it.
--
-- What DOES need defending is the other direction: names that arrive FROM a
-- server. A path in a READDIR reply is attacker-controlled, and a client that
-- joins it onto a local directory without looking can be walked out of that
-- directory. See unsafe_name.
--
-- Still no I/O: builders, parsers, and the framing. The channel carries it.

local wire = require('hull.ssh.wire')

local M = {}

M.VERSION = 3

-- requests
M.FXP_INIT     = 1
M.FXP_VERSION  = 2
M.FXP_OPEN     = 3
M.FXP_CLOSE    = 4
M.FXP_READ     = 5
M.FXP_WRITE    = 6
M.FXP_LSTAT    = 7
M.FXP_FSTAT    = 8
M.FXP_SETSTAT  = 9
M.FXP_OPENDIR  = 11
M.FXP_READDIR  = 12
M.FXP_REMOVE   = 13
M.FXP_MKDIR    = 14
M.FXP_RMDIR    = 15
M.FXP_REALPATH = 16
M.FXP_STAT     = 17
M.FXP_RENAME   = 18

-- responses
M.FXP_STATUS = 101
M.FXP_HANDLE = 102
M.FXP_DATA   = 103
M.FXP_NAME   = 104
M.FXP_ATTRS  = 105

-- status codes
M.FX_OK                = 0
M.FX_EOF               = 1
M.FX_NO_SUCH_FILE      = 2
M.FX_PERMISSION_DENIED = 3
M.FX_FAILURE           = 4
M.FX_BAD_MESSAGE       = 5
M.FX_NO_CONNECTION     = 6
M.FX_CONNECTION_LOST   = 7
M.FX_OP_UNSUPPORTED    = 8

M.STATUS_TEXT = {
    [0] = "ok", [1] = "end of file", [2] = "no such file",
    [3] = "permission denied", [4] = "failure", [5] = "bad message",
    [6] = "no connection", [7] = "connection lost", [8] = "operation unsupported",
}

-- open flags
M.FXF_READ   = 0x01
M.FXF_WRITE  = 0x02
M.FXF_APPEND = 0x04
M.FXF_CREAT  = 0x08
M.FXF_TRUNC  = 0x10
M.FXF_EXCL   = 0x20

-- attribute flags
M.ATTR_SIZE        = 0x01
M.ATTR_UIDGID      = 0x02
M.ATTR_PERMISSIONS = 0x04
M.ATTR_ACMODTIME   = 0x08
M.ATTR_EXTENDED    = 0x80000000

-- A single SFTP packet is bounded well below the SSH packet cap. The largest
-- thing a sane server sends is a READDIR batch or a read of our own chosen
-- size, so a claim much past this is a peer trying to make us hold memory.
M.MAX_PACKET = 256 * 1024

-- Framing -------------------------------------------------------------------

-- SFTP has its own length prefix INSIDE the channel data stream, because
-- channel data is a byte stream with no message boundaries of its own.
function M.frame(payload)
    if #payload > M.MAX_PACKET then
        error("ssh.sftp: packet too large to send", 2)
    end
    return wire.uint32(#payload) .. payload
end

-- Pull one packet off the front of `buf`.
-- Returns payload, consumed; or nil, "need_more" while it is incomplete.
-- Raises on a length that cannot be valid whatever arrives next.
function M.parse_frame(buf)
    if #buf < 4 then return nil, "need_more" end
    local n = string.unpack(">I4", buf, 1)
    -- Bounded BEFORE waiting for the bytes, so a peer claiming 4 GB does not
    -- have us buffering for a packet that will never be legal.
    if n > M.MAX_PACKET then
        error("ssh.sftp: declared packet length " .. tostring(n)
              .. " exceeds the maximum")
    end
    if n < 1 then
        error("ssh.sftp: packet length below the minimum")
    end
    if #buf < n + 4 then return nil, "need_more" end
    return buf:sub(5, 4 + n), n + 4
end

-- Attributes ------------------------------------------------------------------

-- Encode an attribute block. Passing nothing yields the empty block, which is
-- what OPEN wants when the server should choose the mode.
function M.encode_attrs(a)
    local w = wire.writer()
    if not a then
        return w:uint32(0):build()
    end
    local flags = 0
    if a.size then flags = flags | M.ATTR_SIZE end
    if a.uid and a.gid then flags = flags | M.ATTR_UIDGID end
    if a.permissions then flags = flags | M.ATTR_PERMISSIONS end
    if a.atime and a.mtime then flags = flags | M.ATTR_ACMODTIME end
    w:uint32(flags)
    if a.size then w:uint64(a.size) end
    if a.uid and a.gid then w:uint32(a.uid):uint32(a.gid) end
    if a.permissions then w:uint32(a.permissions) end
    if a.atime and a.mtime then w:uint32(a.atime):uint32(a.mtime) end
    return w:build()
end

function M.decode_attrs(r)
    local flags = r:uint32()
    local a = { flags = flags }
    if flags & M.ATTR_SIZE ~= 0 then a.size = r:uint64() end
    if flags & M.ATTR_UIDGID ~= 0 then a.uid = r:uint32(); a.gid = r:uint32() end
    if flags & M.ATTR_PERMISSIONS ~= 0 then a.permissions = r:uint32() end
    if flags & M.ATTR_ACMODTIME ~= 0 then
        a.atime = r:uint32(); a.mtime = r:uint32()
    end
    if flags & M.ATTR_EXTENDED ~= 0 then
        local count = r:uint32()
        -- Each pair is two strings; the reader bounds-checks them, so a
        -- dishonest count runs out of buffer rather than looping forever.
        for _ = 1, count do r:string(); r:string() end
    end
    -- A directory bit, for readdir listings.
    if a.permissions then a.is_dir = (a.permissions & 0xF000) == 0x4000 end
    return a
end

-- Requests ----------------------------------------------------------------------

function M.build_init(version)
    return wire.writer():byte(M.FXP_INIT):uint32(version or M.VERSION):build()
end

local function req(t, id)
    return wire.writer():byte(t):uint32(id)
end

-- The path is a string. It is never split, quoted, or interpreted - which is
-- the whole point of using the subsystem instead of scp over a shell.
function M.build_open(id, path, pflags, attrs)
    return req(M.FXP_OPEN, id):string(path):uint32(pflags)
        :raw(M.encode_attrs(attrs)):build()
end

function M.build_close(id, handle)
    return req(M.FXP_CLOSE, id):string(handle):build()
end

function M.build_read(id, handle, offset, len)
    return req(M.FXP_READ, id):string(handle):uint64(offset):uint32(len):build()
end

function M.build_write(id, handle, offset, data)
    return req(M.FXP_WRITE, id):string(handle):uint64(offset):string(data):build()
end

function M.build_stat(id, path)    return req(M.FXP_STAT, id):string(path):build() end
function M.build_lstat(id, path)   return req(M.FXP_LSTAT, id):string(path):build() end
function M.build_fstat(id, handle) return req(M.FXP_FSTAT, id):string(handle):build() end
function M.build_opendir(id, path) return req(M.FXP_OPENDIR, id):string(path):build() end
function M.build_readdir(id, h)    return req(M.FXP_READDIR, id):string(h):build() end
function M.build_remove(id, path)  return req(M.FXP_REMOVE, id):string(path):build() end
function M.build_rmdir(id, path)   return req(M.FXP_RMDIR, id):string(path):build() end
function M.build_realpath(id, p)   return req(M.FXP_REALPATH, id):string(p):build() end

function M.build_mkdir(id, path, attrs)
    return req(M.FXP_MKDIR, id):string(path):raw(M.encode_attrs(attrs)):build()
end

function M.build_rename(id, from, to)
    return req(M.FXP_RENAME, id):string(from):string(to):build()
end

function M.build_setstat(id, path, attrs)
    return req(M.FXP_SETSTAT, id):string(path):raw(M.encode_attrs(attrs)):build()
end

-- Responses ---------------------------------------------------------------------

function M.parse(payload)
    local r = wire.reader(payload)
    local t = r:byte()

    if t == M.FXP_VERSION then
        local version = r:uint32()
        -- Extensions follow as name/data string pairs; read what is there and
        -- ignore it rather than refusing a server that offers more than we use.
        return { type = "version", version = version }
    end

    local id = r:uint32()

    if t == M.FXP_STATUS then
        local code = r:uint32()
        local out = { type = "status", id = id, code = code,
                      ok = code == M.FX_OK, eof = code == M.FX_EOF }
        -- Version 3 carries a message and language; older servers may not.
        if r:remaining() > 0 then
            out.message = r:string()
            if r:remaining() > 0 then out.language = r:string() end
        end
        out.text = out.message
        if out.text == nil or out.text == "" then
            out.text = M.STATUS_TEXT[code] or ("status " .. tostring(code))
        end
        return out
    end

    if t == M.FXP_HANDLE then
        return { type = "handle", id = id, handle = r:string() }
    end

    if t == M.FXP_DATA then
        return { type = "data", id = id, data = r:string() }
    end

    if t == M.FXP_NAME then
        local count = r:uint32()
        local names = {}
        for i = 1, count do
            local filename = r:string()
            local longname = r:string()
            names[i] = { filename = filename, longname = longname,
                         attrs = M.decode_attrs(r) }
        end
        return { type = "name", id = id, names = names }
    end

    if t == M.FXP_ATTRS then
        return { type = "attrs", id = id, attrs = M.decode_attrs(r) }
    end

    error("ssh.sftp: unexpected response type " .. tostring(t))
end

-- Names from the server --------------------------------------------------------------

-- Whether a filename from a server reply is safe to join onto a local path.
--
-- A READDIR entry is attacker-controlled. A client that takes "../../.ssh/
-- authorized_keys" and writes it under the download directory has been walked
-- out of that directory, and the same goes for an absolute path or an
-- embedded NUL that truncates the name somewhere below.
--
-- This does NOT sanitize: there is no safe rewrite of a hostile name, only a
-- decision to refuse it. Returns nil when the name is usable, or a reason.
function M.unsafe_name(name)
    if type(name) ~= "string" or name == "" then
        return "empty name"
    end
    if name == "." or name == ".." then
        return "special directory entry"
    end
    if name:find("/", 1, true) then
        return "contains a path separator"
    end
    if name:find("\\", 1, true) then
        -- Harmless on a POSIX server, a separator on the Windows host that
        -- may be receiving the file.
        return "contains a backslash"
    end
    if name:find("%z") then
        return "contains a NUL"
    end
    if name:find("[%c]") then
        return "contains a control character"
    end
    return nil
end

-- Filter a READDIR reply to the entries safe to write locally, reporting the
-- rest rather than dropping them silently: a name refused for traversal is
-- something an operator should hear about.
function M.safe_names(names)
    local ok, refused = {}, {}
    for _, e in ipairs(names) do
        local why = M.unsafe_name(e.filename)
        if why then
            refused[#refused + 1] = { filename = e.filename, reason = why }
        else
            ok[#ok + 1] = e
        end
    end
    return ok, refused
end

return M
