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
    local lines = {
        "GET " .. (opts.path or "/") .. " HTTP/1.1",
        "Host: " .. opts.host,
        "Upgrade: websocket",
        "Connection: Upgrade",
        "Sec-WebSocket-Key: " .. opts.key,
        "Sec-WebSocket-Version: 13",
    }
    for _, h in ipairs(opts.headers or {}) do
        -- A header carrying CR or LF would inject a line of its own, and the
        -- values here come from configuration that may come from env.
        if h:find("[\r\n]") then
            error("web.ws-stream: header contains CR or LF: "
                  .. h:gsub("[\r\n]", "?"), 2)
        end
        lines[#lines + 1] = h
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

-- Pull frames until at least one byte of application data is buffered, or the
-- peer closes. Control frames are handled here and never surface.
function Stream:_pump()
    while #self.out == 0 and not self.closed do
        local frame, used = M.decode(self.inbuf)
        if frame then
            self.inbuf = ssub(self.inbuf, used + 1)
            local op = frame.opcode
            if op == M.OP_BIN or op == M.OP_TEXT or op == M.OP_CONT then
                -- A byte stream does not care where the peer put its frame
                -- boundaries, so fragmentation needs no reassembly state:
                -- every data payload is simply more bytes.
                self.out = self.out .. frame.payload
            elseif op == M.OP_PING then
                -- Answering is required (section 5.5.2), and a peer that
                -- pings to check liveness will hang up if we do not.
                self:_send(M.OP_PONG, frame.payload)
            elseif op == M.OP_PONG then           -- unsolicited: ignore
            elseif op == M.OP_CLOSE then
                self.closed = true
                if not self.close_sent then
                    self.close_sent = true
                    pcall(function() self:_send(M.OP_CLOSE, frame.payload) end)
                end
            else
                error("web.ws-stream: unknown opcode " .. tostring(op))
            end
        else
            local chunk, err = self.s:read(self.readsize)
            if chunk == nil then return nil, err end
            if chunk == "" then
                -- The transport ended without a close frame. For a byte
                -- stream that is EOF, not an error: SSH carries its own
                -- disconnect and will report it.
                self.closed = true
            else
                self.inbuf = self.inbuf .. chunk
            end
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
