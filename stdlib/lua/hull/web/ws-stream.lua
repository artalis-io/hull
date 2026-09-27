-- hull.web.ws-stream - a WebSocket client that behaves like a byte stream.
--
-- RFC 6455, client side only. Turns a stream (read/write/close) carrying a
-- WebSocket into a stream carrying the bytes inside it, which is exactly the
-- interface hull.ssh.transport already takes - so SSH over WebSocket needs no
-- change to the SSH code at all, only a different `open_stream`.
--
-- Why this exists: reaching a host through Cloudflare Access means speaking
-- WebSocket to the edge, and `cloudflared access ssh` is a WebSocket carrying
-- raw TCP with two header values for authentication. Observed from cloudflared
-- itself, the upgrade it sends is:
--
--   GET / HTTP/1.1
--   Host: <hostname>
--   Upgrade: websocket
--   Connection: Upgrade
--   Sec-WebSocket-Key: <base64 of 16 random bytes>
--   Sec-WebSocket-Version: 13
--   Cf-Access-Client-Id: <id>.access
--   Cf-Access-Client-Secret: <secret>
--   Cf-Access-Jump-Destination: <host:port>      (only with --destination)
--
-- Nothing about the framing is Cloudflare-specific; the headers are just
-- headers. This module knows about neither SSH nor Cloudflare.
--
-- Sibling to hull/web/ws-client, and deliberately not the same thing. That
-- one is MESSAGE oriented (on_message callbacks) and needs a running server,
-- because it is driven by the event loop. This one is BYTE oriented and owns
-- no loop at all: it reads and writes through a stream the caller supplies,
-- which is what lets it work under app.main, where a fleet tool lives. They
-- share a wire protocol and nothing else.
--
-- It holds no capability. The stream, and the random and sha1 functions, are
-- all passed in - so this module cannot open a connection, only transform one
-- it was handed.

local base64 = require('hull.encoding.base64')

local M = {}

-- RFC 6455 section 1.3. Concatenated with the client key and hashed; its only
-- purpose is to prove the peer understood the handshake rather than echoing.
M.GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

M.OP_CONT  = 0x0
M.OP_TEXT  = 0x1
M.OP_BIN   = 0x2
M.OP_CLOSE = 0x8
M.OP_PING  = 0x9
M.OP_PONG  = 0xA

-- A frame we will hold in memory before deciding the peer is unreasonable.
-- The length field is 64 bits wide and entirely peer-controlled, so this is
-- checked before anything of that size is touched.
M.MAX_FRAME = 4 * 1024 * 1024

-- Control frames carry at most 125 bytes and are never fragmented
-- (RFC 6455 section 5.5).
M.MAX_CONTROL = 125

local schar, ssub, sbyte = string.char, string.sub, string.byte
local spack, sunpack = string.pack, string.unpack

-- Handshake ------------------------------------------------------------

-- The client nonce: 16 random bytes, base64 WITH padding (section 4.1).
function M.key(random_bytes)
    local raw = random_bytes(16)
    if type(raw) ~= "string" or #raw ~= 16 then
        error("web.ws-stream: random_bytes must return 16 bytes", 2)
    end
    return base64.encode(raw)
end

-- What the server must answer with (section 4.2.2 step 5).
function M.accept(sha1_raw, key)
    return base64.encode(sha1_raw(key .. M.GUID))
end

-- Build the upgrade request. `headers` is an array of "Name: value" strings,
-- kept as an array rather than a map so the order is the caller's and a
-- duplicate name is possible where a protocol wants one.
function M.build_request(opts)
    if type(opts.host) ~= "string" or opts.host == "" then
        error("web.ws-stream: a host is required", 2)
    end
    if type(opts.key) ~= "string" or opts.key == "" then
        error("web.ws-stream: a key is required", 2)
    end
    -- CR or LF anywhere in a value injects a line of its own, so every
    -- interpolated part is checked - not only the caller's extra headers.
    --
    -- The request LINE and the Host header were not, and they are the ones
    -- that matter most here: through a relay the destination is chosen by a
    -- header (Cf-Access-Jump-Destination), so a newline in `path` forges the
    -- field that decides which machine is reached - past a manifest that only
    -- ever saw ssh.connect.hosts. `path` is gated by nothing and resolved by
    -- nothing, which is what makes it the reachable one.
    local function no_crlf(what, v)
        if type(v) == "string" and v:find("[\r\n]") then
            error("web.ws-stream: " .. what .. " contains CR or LF: "
                  .. v:gsub("[\r\n]", "?"), 3)
        end
        return v
    end

    local lines = {
        "GET " .. no_crlf("path", opts.path or "/") .. " HTTP/1.1",
        "Host: " .. no_crlf("host", opts.host),
        "Upgrade: websocket",
        "Connection: Upgrade",
        "Sec-WebSocket-Key: " .. no_crlf("key", opts.key),
        "Sec-WebSocket-Version: 13",
    }
    for _, h in ipairs(opts.headers or {}) do
        lines[#lines + 1] = no_crlf("header", h)
    end
    return table.concat(lines, "\r\n") .. "\r\n\r\n"
end

-- Parse the response head. Returns { status, headers } with header names
-- lowercased, or nil plus a reason.
function M.parse_response(text)
    local head = text:match("^(.-)\r\n\r\n")
    if not head then return nil, "incomplete" end
    local first, rest = head:match("^([^\r\n]*)\r\n?(.*)$")
    if not first then return nil, "malformed status line" end
    local status = tonumber(first:match("^HTTP/1%.[01] (%d%d%d)"))
    if not status then return nil, "malformed status line" end

    local headers = {}
    for line in (rest or ""):gmatch("[^\r\n]+") do
        local name, value = line:match("^([^:]+):%s*(.*)$")
        if name then headers[name:lower()] = value end
    end
    return { status = status, headers = headers }
end

-- Framing --------------------------------------------------------------

-- XOR a payload with the 4-byte mask, in chunks so neither the table nor the
-- argument list grows with the payload.
local function apply_mask(data, mask)
    if #data == 0 then return data end
    local m1, m2, m3, m4 = sbyte(mask, 1, 4)
    local m = { m1, m2, m3, m4 }
    local out, n, i = {}, #data, 1
    while i <= n do
        local j = i + 1023
        if j > n then j = n end
        local b = { sbyte(data, i, j) }
        for k = 1, #b do
            b[k] = b[k] ~ m[((i + k - 2) % 4) + 1]
        end
        out[#out + 1] = schar(table.unpack(b))
        i = j + 1
    end
    return table.concat(out)
end
M.apply_mask = apply_mask

-- Encode one CLIENT frame. RFC 6455 section 5.3: a client MUST mask every
-- frame it sends, and a server MUST close the connection on an unmasked one,
-- so the mask is not optional and not a flag.
function M.encode(opcode, payload, mask)
    payload = payload or ""
    if type(mask) ~= "string" or #mask ~= 4 then
        error("web.ws-stream: a 4-byte mask is required", 2)
    end
    if opcode >= 0x8 and #payload > M.MAX_CONTROL then
        error("web.ws-stream: a control frame carries at most 125 bytes", 2)
    end

    local b1 = 0x80 | (opcode & 0x0F)          -- FIN set; no fragmentation
    local n = #payload
    local head
    if n < 126 then
        head = schar(b1, 0x80 | n)
    elseif n < 0x10000 then
        head = schar(b1, 0x80 | 126) .. spack(">I2", n)
    else
        head = schar(b1, 0x80 | 127) .. spack(">I8", n)
    end
    return head .. mask .. apply_mask(payload, mask)
end

-- How many bytes the frame at the front of `buf` occupies in total, or nil
-- plus "need_more" while even that cannot be told yet.
--
-- Split out so a reader can ask the transport for exactly what is missing
-- instead of guessing: appending each short read onto a growing buffer is
-- quadratic in the frame size, which for a 16 KiB frame arriving in small
-- pieces is hundreds of megabytes of copying for 16 KiB of data. The same
-- mistake, and the same fix, as hull.ssh.transport's fill.
function M.frame_size(buf)
    if #buf < 2 then return nil, "need_more" end
    local len = sbyte(buf, 2) & 0x7F
    local head = 2
    if len == 126 then
        if #buf < 4 then return nil, "need_more" end
        len = sunpack(">I2", buf, 3); head = 4
    elseif len == 127 then
        if #buf < 10 then return nil, "need_more" end
        len = sunpack(">I8", buf, 3); head = 10
        if len < 0 then
            error("web.ws-stream: frame length exceeds the representable range")
        end
    end
    if len > M.MAX_FRAME then
        error("web.ws-stream: frame of " .. tostring(len)
              .. " bytes exceeds the maximum")
    end
    -- A server frame is never masked, so there is no 4-byte mask to allow for;
    -- decode refuses one that is.
    return head + len
end

-- Decode one frame from the front of `buf`.
--
-- Returns frame, consumed. Returns nil, "need_more" while incomplete.
-- Raises on anything a conformant server would not send.
function M.decode(buf)
    if #buf < 2 then return nil, "need_more" end

    local b1, b2 = sbyte(buf, 1, 2)
    local fin    = (b1 & 0x80) ~= 0
    local rsv    = b1 & 0x70
    local opcode = b1 & 0x0F
    local masked = (b2 & 0x80) ~= 0
    local len    = b2 & 0x7F
    local pos    = 3

    if rsv ~= 0 then
        -- No extension was negotiated, so a reserved bit set means the peer
        -- is speaking something we did not agree to.
        error("web.ws-stream: reserved bits set without an extension")
    end
    if masked then
        -- RFC 6455 section 5.1: a server MUST NOT mask. Accepting one would
        -- mean guessing at which side's rules apply.
        error("web.ws-stream: server sent a masked frame")
    end

    if len == 126 then
        if #buf < pos + 1 then return nil, "need_more" end
        len = sunpack(">I2", buf, pos); pos = pos + 2
    elseif len == 127 then
        if #buf < pos + 7 then return nil, "need_more" end
        len = sunpack(">I8", buf, pos); pos = pos + 8
        if len < 0 then
            error("web.ws-stream: frame length exceeds the representable range")
        end
    end

    -- Bounded BEFORE waiting for the bytes, so a peer claiming a gigabyte
    -- costs nothing to refuse.
    if len > M.MAX_FRAME then
        error("web.ws-stream: frame of " .. tostring(len)
              .. " bytes exceeds the maximum")
    end
    if opcode >= 0x8 then
        if len > M.MAX_CONTROL then
            error("web.ws-stream: control frame of " .. tostring(len)
                  .. " bytes exceeds 125")
        end
        if not fin then
            error("web.ws-stream: control frame must not be fragmented")
        end
    end

    if #buf < pos - 1 + len then return nil, "need_more" end
    local payload = ssub(buf, pos, pos + len - 1)
    return { fin = fin, opcode = opcode, payload = payload },
           pos - 1 + len
end

-- The stream adapter ---------------------------------------------------

local Stream = {}
Stream.__index = Stream

-- Ensure the buffer holds at least n bytes, gathering short reads into a
-- table and joining once. Returns false at EOF rather than raising: for a
-- byte stream a transport that simply ends is EOF, and SSH carries its own
-- disconnect.
function Stream:_need(n)
    if #self.inbuf >= n then return true end
    local parts, have = { self.inbuf }, #self.inbuf
    while have < n do
        local chunk, err = self.s:read(n - have)
        if chunk == nil then
            self.inbuf = table.concat(parts)
            return nil, err
        end
        if chunk == "" then
            self.inbuf = table.concat(parts)
            self.closed = true
            return false
        end
        parts[#parts + 1] = chunk
        have = have + #chunk
    end
    self.inbuf = table.concat(parts)
    return true
end

-- Pull frames until at least one byte of application data is buffered, or the
-- peer closes. Control frames are handled here and never surface.
function Stream:_pump()
    while #self.out == 0 and not self.closed do
        -- Ask for the header, then for exactly the frame it describes. Nothing
        -- is appended onto a growing buffer, so a peer that dribbles a 16 KiB
        -- frame out a byte at a time costs 16 KiB of copying, not megabytes.
        local ok, err = self:_need(2)
        if ok == nil then return nil, err end
        if not ok then break end                 -- EOF

        -- Two bytes is only enough when the length is inline. An extended
        -- length needs four or ten, so grow through the header until it can
        -- be read rather than assuming; a header is at most ten bytes, so
        -- this is a handful of tiny reads even from the worst transport.
        local total = M.frame_size(self.inbuf)
        while not total do
            local more; more, err = self:_need(#self.inbuf + 1)
            if more == nil then return nil, err end
            if not more then break end           -- EOF inside the header
            total = M.frame_size(self.inbuf)
        end
        if not total then break end

        local got; got, err = self:_need(total)
        if got == nil then return nil, err end
        if not got then break end                -- EOF mid-frame

        local frame, used = M.decode(self.inbuf)
        self.inbuf = ssub(self.inbuf, used + 1)

        local op = frame.opcode
        if op == M.OP_BIN or op == M.OP_TEXT or op == M.OP_CONT then
            -- A byte stream does not care where the peer put its frame
            -- boundaries, so fragmentation needs no reassembly state: every
            -- data payload is simply more bytes.
            self.out = self.out .. frame.payload
        elseif op == M.OP_PING then
            -- Answering is required (section 5.5.2), and a peer that pings to
            -- check liveness will hang up if we do not.
            self:_send(M.OP_PONG, frame.payload)
        elseif op == M.OP_CLOSE then
            self.closed = true
            if not self.close_sent then
                self.close_sent = true
                pcall(function() self:_send(M.OP_CLOSE, frame.payload) end)
            end
        elseif op ~= M.OP_PONG then
            -- An unsolicited pong is legal and means nothing (5.5.3).
            -- Anything else is an opcode we never agreed to.
            error("web.ws-stream: unknown opcode " .. tostring(op))
        end
    end
    return true
end

function Stream:_send(opcode, payload)
    local mask = self.random(4)
    if type(mask) ~= "string" or #mask ~= 4 then
        error("web.ws-stream: random_bytes must return 4 bytes")
    end
    local ok, err = self.s:write(M.encode(opcode, payload, mask))
    if not ok then error("web.ws-stream: write failed: " .. tostring(err)) end
end

function Stream:read(n)
    if n <= 0 then return "" end
    if #self.out == 0 then
        local ok, err = self:_pump()
        if not ok then return nil, err end
    end
    if #self.out == 0 then return "" end       -- closed, nothing buffered
    local take = n < #self.out and n or #self.out
    local s = ssub(self.out, 1, take)
    self.out = ssub(self.out, take + 1)
    return s
end

function Stream:write(data)
    if #data == 0 then return true end
    if self.closed then return nil, "closed" end
    -- One binary frame per write. The caller above is already sending whole
    -- SSH packets, so this adds no buffering of its own.
    local ok, err = pcall(function() self:_send(M.OP_BIN, data) end)
    if not ok then return nil, tostring(err) end
    return true
end

function Stream:close()
    if not self.close_sent and not self.closed then
        self.close_sent = true
        pcall(function() self:_send(M.OP_CLOSE, spack(">I2", 1000)) end)
    end
    self.closed = true
    if self.s and self.s.close then self.s:close() end
end

-- Perform the upgrade over `stream` and return a byte stream carrying what is
-- inside it. Returns nil plus a reason if the peer does not upgrade.
--
--   opts.host     Host header, and what the peer matches its routing on
--   opts.path     default "/"
--   opts.headers  array of "Name: value"
--   opts.random   function(n) -> n random bytes
--   opts.sha1     function(bytes) -> 20 raw digest bytes
function M.connect(stream, opts)
    if type(opts.random) ~= "function" or type(opts.sha1) ~= "function" then
        error("web.ws-stream: random and sha1 functions are required", 2)
    end

    local key = M.key(opts.random)
    local ok, werr = stream:write(M.build_request({
        host = opts.host, path = opts.path, key = key,
        headers = opts.headers,
    }))
    if not ok then return nil, { code = "io_error", detail = tostring(werr) } end

    -- Read until the end of the header block, bounded: a peer that never
    -- sends the blank line must not make us buffer without limit.
    local buf = ""
    while not buf:find("\r\n\r\n", 1, true) do
        if #buf > 64 * 1024 then
            return nil, { code = "upgrade_failed",
                          detail = "response headers too large" }
        end
        local chunk, err = stream:read(4096)
        if chunk == nil then
            return nil, { code = "io_error", detail = tostring(err) }
        end
        if chunk == "" then
            return nil, { code = "upgrade_failed",
                          detail = "connection closed during the upgrade" }
        end
        buf = buf .. chunk
    end

    local res, perr = M.parse_response(buf)
    if not res then
        return nil, { code = "upgrade_failed", detail = perr }
    end
    if res.status ~= 101 then
        -- 403 here is the normal shape of an Access rejection, so the status
        -- is worth reporting rather than flattening to "failed".
        return nil, { code = "upgrade_refused", status = res.status }
    end
    local got = res.headers["sec-websocket-accept"]
    if got ~= M.accept(opts.sha1, key) then
        -- Proves the peer ran the handshake rather than echoing a 101 back,
        -- which is the whole point of the nonce.
        return nil, { code = "upgrade_failed",
                      detail = "Sec-WebSocket-Accept does not match" }
    end

    local rest = buf:match("\r\n\r\n(.*)$") or ""
    return setmetatable({
        s        = stream,
        random   = opts.random,
        inbuf    = rest,        -- the peer may have framed data immediately
        out      = "",
        readsize = opts.readsize or 8192,
        closed   = false,
        close_sent = false,
    }, Stream)
end

return M
