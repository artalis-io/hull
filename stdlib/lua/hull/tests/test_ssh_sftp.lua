-- test_ssh_sftp.lua - Tests for hull.ssh.sftp
--
-- SFTP version 3. Two things are worth testing beyond the round trips: that a
-- path with shell metacharacters travels intact (which is the point of using
-- the subsystem at all), and that names coming back FROM a server cannot walk
-- a download out of its directory.

local sftp = require('hull.ssh.sftp')
local wire = require('hull.ssh.wire')

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

local function assert_raises(fn, msg)
    local ok = pcall(fn)
    if ok then error((msg or "should have raised") .. " but did not") end
end

-- framing ---------------------------------------------------------------------

test("frame and parse_frame round trip", function()
    local p = sftp.build_init()
    local framed = sftp.frame(p)
    local got, used = sftp.parse_frame(framed)
    assert_eq(got, p)
    assert_eq(used, #framed)
end)

test("parse_frame asks for more while incomplete", function()
    local framed = sftp.frame(sftp.build_stat(1, "/etc/hostname"))
    for n = 0, #framed - 1 do
        local got, err = sftp.parse_frame(framed:sub(1, n))
        assert_eq(got, nil, "partial " .. n)
        assert_eq(err, "need_more")
    end
end)

test("parse_frame leaves trailing packets alone", function()
    -- Channel data is a byte stream, so two packets routinely arrive in one
    -- read and the second must survive intact.
    local pa, pb = sftp.build_stat(1, "/a"), sftp.build_stat(2, "/b")
    local buf = sftp.frame(pa) .. sftp.frame(pb)
    local first, used = sftp.parse_frame(buf)
    assert_eq(first, pa)
    assert_eq(used, #sftp.frame(pa))
    local second = sftp.parse_frame(buf:sub(used + 1))
    assert_eq(second, pb)
end)

test("an impossible declared length is refused immediately", function()
    -- Not "wait for 4 GB then fail": the claim can never be legal.
    assert_raises(function()
        sftp.parse_frame("\255\255\255\255" .. "short")
    end, "huge length")
    assert_raises(function()
        sftp.parse_frame("\0\0\0\0")
    end, "zero length")
end)

test("frame refuses to send an oversized packet", function()
    assert_raises(function()
        sftp.frame(string.rep("x", sftp.MAX_PACKET + 1))
    end, "oversize")
end)

-- requests ------------------------------------------------------------------------

test("init declares version 3", function()
    -- 3 because that is what servers implement; a client insisting on 6
    -- talks to almost nothing.
    local r = wire.reader(sftp.build_init())
    assert_eq(r:byte(), sftp.FXP_INIT)
    assert_eq(r:uint32(), 3)
end)

test("a path with shell metacharacters travels intact", function()
    -- The reason file transfer needs no quoting: the path is a length-
    -- prefixed string in a binary protocol, never a word in a command line.
    local nasty = [[/tmp/; rm -rf / && echo $(whoami) `id` 'quoted' "double"]]
    local r = wire.reader(sftp.build_open(1, nasty, sftp.FXF_READ))
    assert_eq(r:byte(), sftp.FXP_OPEN)
    assert_eq(r:uint32(), 1)
    assert_eq(r:string(), nasty, "byte for byte")
end)

test("a path with a newline or NUL travels intact too", function()
    local weird = "/tmp/two\nlines\0and-a-nul"
    local r = wire.reader(sftp.build_stat(5, weird))
    r:byte(); r:uint32()
    assert_eq(r:string(), weird)
end)

test("open carries flags and an empty attribute block", function()
    local r = wire.reader(sftp.build_open(9, "/f",
        sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_TRUNC))
    r:byte(); r:uint32(); r:string()
    assert_eq(r:uint32(), 0x02 | 0x08 | 0x10)
    assert_eq(r:uint32(), 0, "empty attrs: let the server pick the mode")
end)

test("read names an offset and a length", function()
    local r = wire.reader(sftp.build_read(2, "H", 4294967296, 32768))
    assert_eq(r:byte(), sftp.FXP_READ)
    assert_eq(r:uint32(), 2)
    assert_eq(r:string(), "H")
    assert_eq(r:uint64(), 4294967296, "offsets are 64-bit")
    assert_eq(r:uint32(), 32768)
end)

test("write carries data at an offset", function()
    local r = wire.reader(sftp.build_write(3, "H", 100, "payload"))
    r:byte(); r:uint32()
    assert_eq(r:string(), "H")
    assert_eq(r:uint64(), 100)
    assert_eq(r:string(), "payload")
end)

test("rename carries both paths", function()
    local r = wire.reader(sftp.build_rename(4, "/from", "/to"))
    r:byte(); r:uint32()
    assert_eq(r:string(), "/from")
    assert_eq(r:string(), "/to")
end)

test("mkdir can set a mode", function()
    local r = wire.reader(sftp.build_mkdir(5, "/d", { permissions = 0x1ED }))
    r:byte(); r:uint32(); r:string()
    assert_eq(r:uint32(), sftp.ATTR_PERMISSIONS)
    assert_eq(r:uint32(), 0x1ED)
end)

-- responses -------------------------------------------------------------------------

test("version response is parsed", function()
    local p = wire.writer():byte(sftp.FXP_VERSION):uint32(3):build()
    local m = sftp.parse(p)
    assert_eq(m.type, "version")
    assert_eq(m.version, 3)
end)

test("status ok and eof are distinguished", function()
    -- EOF is how a read loop ends; treating it as an error would make every
    -- complete download look like a failure.
    local ok = sftp.parse(wire.writer():byte(101):uint32(1):uint32(0)
        :string(""):string(""):build())
    assert_eq(ok.ok, true)
    assert_eq(ok.eof, false)

    local eof = sftp.parse(wire.writer():byte(101):uint32(1):uint32(1)
        :string(""):string(""):build())
    assert_eq(eof.ok, false)
    assert_eq(eof.eof, true)
end)

test("status falls back to a readable text", function()
    local m = sftp.parse(wire.writer():byte(101):uint32(1):uint32(3)
        :string(""):string(""):build())
    assert_eq(m.text, "permission denied")
end)

test("a server message is preferred over our text", function()
    local m = sftp.parse(wire.writer():byte(101):uint32(1):uint32(4)
        :string("disk full"):string("en"):build())
    assert_eq(m.text, "disk full")
end)

test("a status with no message still has text", function()
    -- Older servers omit the message and language entirely.
    local m = sftp.parse(wire.writer():byte(101):uint32(1):uint32(2):build())
    assert_eq(m.text, "no such file")
end)

test("handle and data responses are parsed", function()
    local h = sftp.parse(wire.writer():byte(102):uint32(1):string("HANDLE"):build())
    assert_eq(h.handle, "HANDLE")
    local d = sftp.parse(wire.writer():byte(103):uint32(1):string("bytes"):build())
    assert_eq(d.data, "bytes")
end)

test("a name reply is parsed with attributes", function()
    local p = wire.writer():byte(104):uint32(1):uint32(2)
        :string("a.txt"):string("-rw-r--r-- ...")
            :uint32(sftp.ATTR_SIZE):uint64(1234)
        :string("sub"):string("drwxr-xr-x ...")
            :uint32(sftp.ATTR_PERMISSIONS):uint32(0x41ED)
        :build()
    local m = sftp.parse(p)
    assert_eq(m.type, "name")
    assert_eq(#m.names, 2)
    assert_eq(m.names[1].filename, "a.txt")
    assert_eq(m.names[1].attrs.size, 1234)
    assert_eq(m.names[2].attrs.is_dir, true)
end)

test("attrs decode only the flagged fields", function()
    local blob = sftp.encode_attrs({ size = 42, permissions = 0x1A4 })
    local a = sftp.decode_attrs(wire.reader(blob))
    assert_eq(a.size, 42)
    assert_eq(a.permissions, 0x1A4)
    assert_eq(a.uid, nil)
    assert_eq(a.mtime, nil)
end)

test("a dishonest extended count runs out of buffer", function()
    -- Rather than looping on a count the packet cannot back up.
    local blob = wire.writer():uint32(sftp.ATTR_EXTENDED):uint32(1000):build()
    assert_raises(function()
        sftp.decode_attrs(wire.reader(blob))
    end, "lying extended count")
end)

test("an unexpected response type raises", function()
    assert_raises(function()
        sftp.parse(wire.writer():byte(200):uint32(1):build())
    end, "type 200")
end)

-- names from the server ------------------------------------------------------------------

test("an ordinary name is usable", function()
    assert_eq(sftp.unsafe_name("report.txt"), nil)
    assert_eq(sftp.unsafe_name("with spaces and-dashes.tar.gz"), nil)
end)

test("traversal names are refused", function()
    -- A client that joins this onto a download directory has been walked out
    -- of it.
    assert_eq(sftp.unsafe_name("..") ~= nil, true)
    assert_eq(sftp.unsafe_name(".") ~= nil, true)
    assert_eq(sftp.unsafe_name("../../.ssh/authorized_keys") ~= nil, true)
    assert_eq(sftp.unsafe_name("/etc/passwd") ~= nil, true)
    assert_eq(sftp.unsafe_name("sub/file") ~= nil, true)
end)

test("a backslash is refused too", function()
    -- Harmless on the POSIX server, a separator on the Windows host that may
    -- be receiving the file.
    assert_eq(sftp.unsafe_name([[..\..\evil]]) ~= nil, true)
end)

test("NUL and control characters are refused", function()
    assert_eq(sftp.unsafe_name("ok\0/etc/passwd") ~= nil, true)
    assert_eq(sftp.unsafe_name("clear\27[2Jscreen") ~= nil, true)
    assert_eq(sftp.unsafe_name("") ~= nil, true)
end)

test("safe_names reports what it refused rather than dropping it", function()
    -- A name refused for traversal is something an operator should hear
    -- about, not something to silently omit from a listing.
    local ok, refused = sftp.safe_names({
        { filename = "good.txt" },
        { filename = "../escape" },
        { filename = "also-good" },
        { filename = "bad\0name" },
    })
    assert_eq(#ok, 2)
    assert_eq(ok[1].filename, "good.txt")
    assert_eq(#refused, 2)
    assert_eq(refused[1].filename, "../escape")
    assert_eq(type(refused[1].reason), "string")
end)

test("unsafe_name does not try to sanitize", function()
    -- There is no safe rewrite of a hostile name, only a decision to refuse
    -- it. The contract is a reason or nil, never a cleaned-up string.
    local r = sftp.unsafe_name("../x")
    assert_eq(type(r), "string")
    assert_eq(r ~= "../x", true)
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
