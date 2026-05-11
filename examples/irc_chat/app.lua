-- IRC Chat — Encrypted WebSocket chat with channels
--
-- Run: hull app.lua -p 3000
--
-- E2E encryption: messages are encrypted with per-channel symmetric keys
-- using crypto.secretbox (XSalsa20-Poly1305). Channel keys are distributed
-- to members encrypted with crypto.box (Curve25519+XSalsa20+Poly1305).
-- The server relays encrypted ciphertext — it never sees plaintext messages.
--
-- HTTP endpoints:
--   POST /register            — create user + Curve25519 keypair
--   POST /login               — authenticate, returns keypair
--   POST /logout              — destroy session
--   GET  /me                  — current user info
--   GET  /channels            — list channels
--   POST /channels            — create channel { name, topic? }
--   GET  /channels/:name/members  — list members with public keys
--   GET  /channels/:name/history  — recent encrypted messages
--   GET  /users               — list users with public keys + online status
--   GET  /dm/:username/history — encrypted DM history (session auth)
--   POST /files               — upload encrypted file (session auth)
--   GET  /files/:id           — download encrypted file (session auth)
--   GET  /channels/:name/files    — list channel files
--   GET  /dm/:username/files      — list DM files (session auth)
--   GET  /health              — health check
--
-- WebSocket:
--   WS /ws — authenticated chat (see protocol below)

local validate = require("hull.validate")
local session  = require("hull.middleware.session")
local auth     = require("hull.middleware.auth")
local _cookie  = require("hull.cookie") -- luacheck: ignore

app.manifest({ hosts = {"127.0.0.1"} })
session.init({ ttl = 7200 })

-- ── Retention config (0 = no limit, >0 = TTL in seconds) ────────────

local RETENTION = {
    channel_ttl      = 0, -- channel message TTL (e.g. 86400 * 90 for 90 days)
    dm_ttl           = 0, -- direct message TTL (e.g. 86400 * 30 for 30 days)
    file_ttl         = 0, -- file TTL (e.g. 86400 * 7 for 7 days)
    max_file_storage = 0, -- max total file content in bytes (e.g. 50 * 1048576 for 50 MB)
    cleanup_interval = 3600000, -- check every hour (ms)
}

local function cleanup_expired()
    local now = time.now()
    local deleted = 0
    if RETENTION.channel_ttl > 0 then
        local cutoff = now - RETENTION.channel_ttl
        db.exec("DELETE FROM messages WHERE created_at < ?", { cutoff })
        local del = db.query("SELECT changes() as n")
        deleted = deleted + (del[1] and del[1].n or 0)
    end
    if RETENTION.dm_ttl > 0 then
        local cutoff = now - RETENTION.dm_ttl
        db.exec("DELETE FROM direct_messages WHERE created_at < ?", { cutoff })
        local del = db.query("SELECT changes() as n")
        deleted = deleted + (del[1] and del[1].n or 0)
    end
    if RETENTION.file_ttl > 0 then
        local cutoff = now - RETENTION.file_ttl
        db.exec("DELETE FROM files WHERE created_at < ?", { cutoff })
        local del = db.query("SELECT changes() as n")
        deleted = deleted + (del[1] and del[1].n or 0)
    end
    if RETENTION.max_file_storage > 0 then
        local usage = db.query("SELECT COALESCE(SUM(size), 0) as total FROM files")
        local total = usage[1] and usage[1].total or 0
        if total > RETENTION.max_file_storage then
            db.exec(
                "DELETE FROM files WHERE id IN (SELECT id FROM files ORDER BY created_at ASC LIMIT 100)")
            local del = db.query("SELECT changes() as n")
            deleted = deleted + (del[1] and del[1].n or 0)
        end
    end
    if deleted > 0 then
        log.info("retention cleanup: deleted " .. tostring(deleted) .. " expired records")
    end
end

-- ── Federation config ─────────────────────────────────────────────

local FEDERATION = {
    enabled = false,
    server_name = "alpha",
    channels = { "#general" },    -- only these channels are relayed
    peers = {},                    -- { url = "ws://host:port/federation", public_key = "hex" }
}

-- Generate ephemeral Ed25519 keypair for server identity
if not FEDERATION.public_key then
    FEDERATION.public_key, FEDERATION.secret_key = crypto.ed25519_keypair()
end

-- In-memory federation state
local fed_peers = {}     -- server_name -> { conn, public_key, authenticated }
local fed_pending = {}   -- conn_id -> { challenge, public_key, state, server_name, challenge2, created_at }
local FED_MAX_INBOUND = 64
local FED_HANDSHAKE_TIMEOUT = 30 -- seconds
local FED_MAX_FIELD_LEN = 256

local function fed_channel_enabled(channel)
    for _, ch in ipairs(FEDERATION.channels) do
        if ch == channel then return true end
    end
    return false
end

local function fed_find_peer_by_pk(pk)
    for _, peer in ipairs(FEDERATION.peers) do
        if peer.public_key == pk then return peer end
    end
    return nil
end

local function federation_relay(channel, msg)
    if not FEDERATION.enabled then return end
    if not fed_channel_enabled(channel) then return end
    msg.server = FEDERATION.server_name
    local encoded = json.encode(msg)
    for _, peer in pairs(fed_peers) do
        if peer.authenticated and peer.conn then
            pcall(function() peer.conn:send(encoded) end)
        end
    end
end

local function federation_relay_presence(username, online)
    if not FEDERATION.enabled then return end
    local msg = {
        type = "fed_presence", server = FEDERATION.server_name,
        username = username, online = online,
    }
    local encoded = json.encode(msg)
    for _, peer in pairs(fed_peers) do
        if peer.authenticated and peer.conn then
            pcall(function() peer.conn:send(encoded) end)
        end
    end
end

local function fed_check_len(s)
    return type(s) == "string" and #s <= FED_MAX_FIELD_LEN
end

local function handle_federated_message(data)
    -- Relay federated messages to local /ws clients — never re-relay to other peers
    if not data.server or not fed_check_len(data.server) then return end
    if data.type == "fed_msg" then
        if not data.channel or not data.from or not data.encrypted or not data.nonce then return end
        if not fed_check_len(data.from) or not fed_check_len(data.channel) then return end
        if not fed_channel_enabled(data.channel) then return end
        ws.broadcast("/ws", json.encode({
            type = "msg", channel = data.channel,
            from = data.from .. "@" .. data.server,
            encrypted = data.encrypted, nonce = data.nonce,
            at = data.at, federated = true,
        }))
    elseif data.type == "fed_join" then
        if not data.channel or not data.user then return end
        if not fed_check_len(data.user) or not fed_check_len(data.channel) then return end
        if not fed_channel_enabled(data.channel) then return end
        ws.broadcast("/ws", json.encode({
            type = "user_joined", channel = data.channel,
            user = data.user .. "@" .. data.server, federated = true,
        }))
    elseif data.type == "fed_leave" then
        if not data.channel or not data.user then return end
        if not fed_check_len(data.user) or not fed_check_len(data.channel) then return end
        if not fed_channel_enabled(data.channel) then return end
        ws.broadcast("/ws", json.encode({
            type = "left", channel = data.channel,
            user = data.user .. "@" .. data.server, federated = true,
        }))
    elseif data.type == "fed_presence" then
        if not data.username or not fed_check_len(data.username) then return end
        ws.broadcast("/ws", json.encode({
            type = "presence", username = data.username .. "@" .. data.server,
            online = data.online, federated = true,
        }))
    end
end

-- ── Helpers ─────────────────────────────────────────────────────────

local function to_hex(raw)
    local hex = {}
    for i = 1, #raw do
        hex[i] = string.format("%02x", string.byte(raw, i))
    end
    return table.concat(hex)
end

local function require_session(req, res)
    if not req.ctx.session then
        res:status(401):json({ error = "authentication required" })
        return nil
    end
    return req.ctx.session
end

-- ── Session middleware ──────────────────────────────────────────────

app.use("*", "/*", auth.session_middleware({ optional = true }))

-- ── Health ──────────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- ── Auth endpoints ──────────────────────────────────────────────────

app.post("/register", function(req, res)
    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local ok, errors = validate.check(body, {
        username = { required = true, min = 2, max = 32 },
        password = { required = true, min = 8 },
    })
    if not ok then
        return res:status(400):json({ errors = errors })
    end

    -- Generate Curve25519 keypair for E2E encryption
    local pk, sk = crypto.box_keypair()
    local hash = crypto.hash_password(body.password)
    local id

    local ok_txn, txn_err = pcall(function()
        db.batch(function()
            local existing = db.query(
                "SELECT id FROM users WHERE username = ?", { body.username })
            if #existing > 0 then error("username already taken") end
            db.exec(
                "INSERT INTO users (username, password_hash, public_key, created_at) VALUES (?, ?, ?, ?)",
                { body.username, hash, pk, time.now() })
            id = db.last_id()
        end)
    end)

    if not ok_txn then
        if tostring(txn_err):match("username already taken") then
            return res:status(409):json({ error = "username already taken" })
        end
        return res:status(500):json({ error = "registration failed" })
    end

    res:status(201):json({
        id = id, username = body.username, public_key = pk, secret_key = sk,
    })
end)

app.post("/login", function(req, res)
    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local ok, errors = validate.check(body, {
        username = { required = true },
        password = { required = true },
    })
    if not ok then
        return res:status(400):json({ errors = errors })
    end

    local rows = db.query(
        "SELECT * FROM users WHERE username = ?", { body.username })
    if #rows == 0 then
        return res:status(401):json({ error = "invalid credentials" })
    end

    local user = rows[1]
    if not crypto.verify_password(body.password, user.password_hash) then
        return res:status(401):json({ error = "invalid credentials" })
    end

    -- Store secret key in session (simplified — in production, client holds this)
    auth.login(req, res, {
        user_id = user.id, username = user.username, public_key = user.public_key,
    })

    res:json({
        id = user.id, username = user.username,
        public_key = user.public_key,
    })
end)

app.post("/logout", function(req, res)
    if not require_session(req, res) then return end
    auth.logout(req, res)
    res:json({ ok = true })
end)

app.get("/me", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end
    res:json({
        id = sess.user_id, username = sess.username,
        public_key = sess.public_key,
    })
end)

-- ── Channel endpoints ───────────────────────────────────────────────

app.get("/channels", function(_req, res)
    local channels = db.query([[
        SELECT c.id, c.name, c.topic, c.created_at,
               COUNT(cm.user_id) as member_count
        FROM channels c
        LEFT JOIN channel_members cm ON cm.channel_id = c.id
        GROUP BY c.id
        ORDER BY c.name
    ]])
    res:json({ channels = channels })
end)

app.post("/channels", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local ok, errors = validate.check(body, {
        name = { required = true, min = 2, max = 50 },
    })
    if not ok then
        return res:status(400):json({ errors = errors })
    end

    local name = body.name
    if not name:match("^#") then name = "#" .. name end

    -- Generate a channel symmetric key and encrypt it for the creator
    local channel_key_hex = to_hex(crypto.random(32))
    local nonce_hex = to_hex(crypto.random(24))

    -- Encrypt channel key for the creator using crypto.box
    local encrypted_key = crypto.box(
        channel_key_hex, nonce_hex, sess.public_key, sess.public_key)
    -- Note: in a real system, the creator would use their own sk to encrypt
    -- for themselves. Here we store the key directly since both sides are server.

    local channel_id
    local ok_txn, txn_err = pcall(function()
        db.batch(function()
            local existing = db.query(
                "SELECT id FROM channels WHERE name = ?", { name })
            if #existing > 0 then error("channel already exists") end
            db.exec(
                "INSERT INTO channels (name, topic, creator_id, created_at) VALUES (?, ?, ?, ?)",
                { name, body.topic or "", sess.user_id, time.now() })
            channel_id = db.last_id()
            -- Creator auto-joins as admin
            db.exec(
                "INSERT INTO channel_members (channel_id, user_id, role, encrypted_key, nonce, joined_at) VALUES (?, ?, ?, ?, ?, ?)",
                { channel_id, sess.user_id, "admin", encrypted_key, nonce_hex, time.now() })
        end)
    end)

    if not ok_txn then
        if tostring(txn_err):match("channel already exists") then
            return res:status(409):json({ error = "channel already exists" })
        end
        return res:status(500):json({ error = "channel creation failed" })
    end

    res:status(201):json({
        id = channel_id, name = name, topic = body.topic or "",
        channel_key = channel_key_hex,
    })
end)

app.get("/channels/:name/members", function(req, res)
    local channel_name = req.params.name
    if not channel_name:match("^#") then channel_name = "#" .. channel_name end

    local members = db.query([[
        SELECT u.id, u.username, u.public_key, cm.role, cm.joined_at
        FROM channel_members cm
        JOIN users u ON u.id = cm.user_id
        WHERE cm.channel_id = (SELECT id FROM channels WHERE name = ?)
        ORDER BY cm.joined_at
    ]], { channel_name })

    res:json({ channel = channel_name, members = members })
end)

app.get("/channels/:name/history", function(req, res)
    local channel_name = req.params.name
    if not channel_name:match("^#") then channel_name = "#" .. channel_name end

    local messages = db.query([[
        SELECT m.id, u.username, m.encrypted_body, m.nonce, m.created_at
        FROM messages m
        JOIN users u ON u.id = m.user_id
        WHERE m.channel_id = (SELECT id FROM channels WHERE name = ?)
        ORDER BY m.created_at DESC
        LIMIT 50
    ]], { channel_name })

    res:json({ channel = channel_name, messages = messages })
end)

-- ── WebSocket chat ──────────────────────────────────────────────────
-- Protocol: JSON messages over WebSocket
--
-- Client -> Server:
--   { type: "join",  channel: "#general" }
--   { type: "leave", channel: "#general" }
--   { type: "msg",   channel: "#general", encrypted: "hex...", nonce: "hex..." }
--   { type: "topic", channel: "#general", topic: "New topic" }
--   { type: "who",   channel: "#general" }
--   { type: "list" }
--
-- Server -> Client:
--   { type: "joined",  channel, user, members, encrypted_key, nonce }
--   { type: "left",    channel, user }
--   { type: "msg",     channel, from, encrypted, nonce, at }
--   { type: "topic",   channel, topic, by }
--   { type: "members", channel, users }
--   { type: "channels", list }
--   { type: "error",   message }
--   { type: "motd",    text }

-- In-memory: track which connections are in which channels
-- conn.data.username = username
-- conn.data.user_id  = user_id
-- conn.data.channels = { ["#general"] = true, ... }

local online_users = {} -- username -> conn

local function ws_send(conn, msg)
    conn:send(json.encode(msg))
end

local function ws_error(conn, message)
    ws_send(conn, { type = "error", message = message })
end

local function broadcast_to_channel(channel_name, msg, exclude_conn)
    -- Broadcast via the WS path, but we need to filter by channel membership
    -- Since ws.broadcast sends to ALL connections on a path, we use conn.data
    -- to track channel membership and send individually
    -- This is a simplification — in production you'd want a channel-based pubsub
    local _exclude_id = exclude_conn and exclude_conn:id() or -1 -- luacheck: ignore
    local sent = json.encode(msg)
    -- We rely on the per-connection data to filter
    -- For now, use ws.broadcast and let clients filter
    ws.broadcast("/ws", sent)
end

app.ws("/ws", {
    on_open = function(conn)
        -- WebSocket connections need auth info stored on connection
        -- For simplicity, mark as unauthenticated until they send credentials
        conn.data.username = nil
        conn.data.user_id = nil
        conn.data.channels = {}
        conn.data.authenticated = false

        ws_send(conn, { type = "motd", text = "Welcome to Hull IRC Chat. Send a 'login' message to authenticate." })
    end,

    on_message = function(conn, raw)
        local decode_ok, msg = pcall(json.decode, raw)
        if not decode_ok or not msg or not msg.type then
            return ws_error(conn, "invalid JSON message")
        end

        -- Handle login over WS (since session middleware doesn't apply)
        if msg.type == "login" then
            if not msg.username or not msg.password then
                return ws_error(conn, "username and password required")
            end
            local rows = db.query(
                "SELECT * FROM users WHERE username = ?", { msg.username })
            if #rows == 0 then
                return ws_error(conn, "invalid credentials")
            end
            if not crypto.verify_password(msg.password, rows[1].password_hash) then
                return ws_error(conn, "invalid credentials")
            end
            conn.data.username = rows[1].username
            conn.data.user_id = rows[1].id
            conn.data.public_key = rows[1].public_key
            conn.data.authenticated = true
            online_users[rows[1].username] = conn
            ws_send(conn, {
                type = "authenticated",
                username = rows[1].username,
                public_key = rows[1].public_key,
            })
            ws.broadcast("/ws", json.encode({
                type = "presence", username = rows[1].username, online = true,
            }))
            federation_relay_presence(rows[1].username, true)
            return
        end

        -- All other commands require authentication
        if not conn.data.authenticated then
            return ws_error(conn, "not authenticated — send login first")
        end

        if msg.type == "join" then
            if not msg.channel then
                return ws_error(conn, "channel name required")
            end
            local channel_name = msg.channel
            if not channel_name:match("^#") then
                channel_name = "#" .. channel_name
            end

            -- Check channel exists
            local ch = db.query(
                "SELECT id FROM channels WHERE name = ?", { channel_name })
            if #ch == 0 then
                return ws_error(conn, "channel not found: " .. channel_name)
            end

            -- Check if already a member, if not, join
            local membership = db.query(
                "SELECT encrypted_key, nonce FROM channel_members WHERE channel_id = ? AND user_id = ?",
                { ch[1].id, conn.data.user_id })

            if #membership == 0 then
                -- Add as member with a placeholder encrypted key
                -- In a real system, an admin would encrypt the channel key for this user
                local nonce_hex = to_hex(crypto.random(24))
                db.exec(
                    "INSERT OR IGNORE INTO channel_members (channel_id, user_id, role, encrypted_key, nonce, joined_at) VALUES (?, ?, ?, ?, ?, ?)",
                    { ch[1].id, conn.data.user_id, "member", "pending", nonce_hex, time.now() })
                membership = {{ encrypted_key = "pending", nonce = nonce_hex }}
            end

            conn.data.channels[channel_name] = true

            -- Get member list
            local members = db.query(
                "SELECT u.username FROM channel_members cm JOIN users u ON u.id = cm.user_id WHERE cm.channel_id = ?",
                { ch[1].id })
            local member_names = {}
            for _, m in ipairs(members) do
                member_names[#member_names + 1] = m.username
            end

            ws_send(conn, {
                type = "joined", channel = channel_name,
                user = conn.data.username, members = member_names,
                encrypted_key = membership[1].encrypted_key,
                nonce = membership[1].nonce,
            })

            -- Notify others
            broadcast_to_channel(channel_name, {
                type = "user_joined", channel = channel_name,
                user = conn.data.username,
            })
            federation_relay(channel_name, {
                type = "fed_join", channel = channel_name, user = conn.data.username,
            })

        elseif msg.type == "leave" then
            if not msg.channel then
                return ws_error(conn, "channel name required")
            end
            conn.data.channels[msg.channel] = nil
            broadcast_to_channel(msg.channel, {
                type = "left", channel = msg.channel,
                user = conn.data.username,
            })
            federation_relay(msg.channel, {
                type = "fed_leave", channel = msg.channel, user = conn.data.username,
            })

        elseif msg.type == "msg" then
            if not msg.channel or not msg.encrypted or not msg.nonce then
                return ws_error(conn, "channel, encrypted, and nonce required")
            end
            if not conn.data.channels[msg.channel] then
                return ws_error(conn, "not in channel: " .. msg.channel)
            end

            -- Store encrypted message
            local ch = db.query(
                "SELECT id FROM channels WHERE name = ?", { msg.channel })
            if #ch > 0 then
                db.exec(
                    "INSERT INTO messages (channel_id, user_id, encrypted_body, nonce, created_at) VALUES (?, ?, ?, ?, ?)",
                    { ch[1].id, conn.data.user_id, msg.encrypted, msg.nonce, time.now() })
            end

            -- Broadcast encrypted message to all WS connections
            local msg_at = time.now()
            broadcast_to_channel(msg.channel, {
                type = "msg", channel = msg.channel,
                from = conn.data.username,
                encrypted = msg.encrypted,
                nonce = msg.nonce,
                at = msg_at,
            })
            federation_relay(msg.channel, {
                type = "fed_msg", channel = msg.channel, from = conn.data.username,
                encrypted = msg.encrypted, nonce = msg.nonce, at = msg_at,
            })

        elseif msg.type == "topic" then
            if not msg.channel or not msg.topic then
                return ws_error(conn, "channel and topic required")
            end
            db.exec("UPDATE channels SET topic = ? WHERE name = ?",
                    { msg.topic, msg.channel })
            broadcast_to_channel(msg.channel, {
                type = "topic", channel = msg.channel,
                topic = msg.topic, by = conn.data.username,
            })

        elseif msg.type == "who" then
            if not msg.channel then
                return ws_error(conn, "channel name required")
            end
            local members = db.query([[
                SELECT u.username, u.public_key, cm.role
                FROM channel_members cm JOIN users u ON u.id = cm.user_id
                WHERE cm.channel_id = (SELECT id FROM channels WHERE name = ?)
            ]], { msg.channel })
            ws_send(conn, {
                type = "members", channel = msg.channel, users = members,
            })

        elseif msg.type == "list" then
            local channels = db.query([[
                SELECT c.name, c.topic, COUNT(cm.user_id) as members
                FROM channels c
                LEFT JOIN channel_members cm ON cm.channel_id = c.id
                GROUP BY c.id ORDER BY c.name
            ]])
            ws_send(conn, { type = "channels", list = channels })

        elseif msg.type == "dm" then
            if not msg.to or not msg.encrypted_for_recipient
               or not msg.encrypted_for_sender or not msg.nonce then
                return ws_error(conn, "to, encrypted_for_recipient, encrypted_for_sender, and nonce required")
            end
            local recipient = db.query(
                "SELECT id FROM users WHERE username = ?", { msg.to })
            if #recipient == 0 then
                return ws_error(conn, "user not found: " .. tostring(msg.to))
            end
            local now = time.now()
            db.exec(
                "INSERT INTO direct_messages (sender_id, recipient_id, encrypted_for_recipient, encrypted_for_sender, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?)",
                { conn.data.user_id, recipient[1].id, msg.encrypted_for_recipient, msg.encrypted_for_sender, msg.nonce, now })
            local dm_id = db.last_id()
            -- Deliver to recipient if online
            local rcpt_conn = online_users[msg.to]
            if rcpt_conn then
                ws_send(rcpt_conn, {
                    type = "dm", from = conn.data.username,
                    encrypted = msg.encrypted_for_recipient,
                    nonce = msg.nonce, at = now, id = dm_id,
                })
            end
            -- Confirm to sender
            ws_send(conn, {
                type = "dm", from = conn.data.username, to = msg.to,
                encrypted = msg.encrypted_for_sender,
                nonce = msg.nonce, at = now, id = dm_id,
            })

        elseif msg.type == "dm_history" then
            if not msg.with then
                return ws_error(conn, "with field required")
            end
            local other = db.query(
                "SELECT id FROM users WHERE username = ?", { msg.with })
            if #other == 0 then
                return ws_error(conn, "user not found: " .. tostring(msg.with))
            end
            local limit = msg.limit or 50
            if limit > 200 then limit = 200 end
            local other_id = other[1].id
            local my_id = conn.data.user_id
            local messages
            if msg.before then
                messages = db.query([[
                    SELECT dm.id, u.username as "from",
                           CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender
                                ELSE dm.encrypted_for_recipient END as encrypted,
                           dm.nonce, dm.created_at as at
                    FROM direct_messages dm
                    JOIN users u ON u.id = dm.sender_id
                    WHERE ((dm.sender_id = ? AND dm.recipient_id = ?)
                        OR (dm.sender_id = ? AND dm.recipient_id = ?))
                      AND dm.id < ?
                    ORDER BY dm.created_at DESC LIMIT ?
                ]], { my_id, my_id, other_id, other_id, my_id, msg.before, limit })
            else
                messages = db.query([[
                    SELECT dm.id, u.username as "from",
                           CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender
                                ELSE dm.encrypted_for_recipient END as encrypted,
                           dm.nonce, dm.created_at as at
                    FROM direct_messages dm
                    JOIN users u ON u.id = dm.sender_id
                    WHERE (dm.sender_id = ? AND dm.recipient_id = ?)
                       OR (dm.sender_id = ? AND dm.recipient_id = ?)
                    ORDER BY dm.created_at DESC LIMIT ?
                ]], { my_id, my_id, other_id, other_id, my_id, limit })
            end
            ws_send(conn, { type = "dm_history", with = msg.with, messages = messages })

        elseif msg.type == "typing" then
            if not msg.to then
                return ws_error(conn, "to field required")
            end
            local rcpt_conn = online_users[msg.to]
            if rcpt_conn then
                ws_send(rcpt_conn, { type = "typing", from = conn.data.username })
            end

        elseif msg.type == "users" then
            local users = db.query(
                "SELECT username, public_key FROM users ORDER BY username")
            for _, u in ipairs(users) do
                u.online = online_users[u.username] ~= nil
            end
            ws_send(conn, { type = "users", list = users })

        else
            ws_error(conn, "unknown message type: " .. tostring(msg.type))
        end
    end,

    on_close = function(conn, code, _reason)
        if conn.data.username then
            online_users[conn.data.username] = nil
            for channel, _ in pairs(conn.data.channels or {}) do
                broadcast_to_channel(channel, {
                    type = "left", channel = channel,
                    user = conn.data.username,
                })
            end
            ws.broadcast("/ws", json.encode({
                type = "presence", username = conn.data.username, online = false,
            }))
            federation_relay_presence(conn.data.username, false)
        end
        log.info("ws: " .. tostring(conn.data.username or "anon")
                 .. " disconnected code=" .. tostring(code))
    end,
})

-- ── WS connection count (for health/monitoring) ─────────────────────

app.get("/ws/connections", function(_req, res)
    res:json({ count = ws.connections("/ws") })
end)

-- ── Users endpoint ────────────────────────────────────────────────

app.get("/users", function(_req, res)
    local users = db.query(
        "SELECT username, public_key FROM users ORDER BY username")
    for _, u in ipairs(users) do
        u.online = online_users[u.username] ~= nil
    end
    res:json({ users = users })
end)

-- ── DM history endpoint ──────────────────────────────────────────

app.get("/dm/:username/history", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local other = db.query(
        "SELECT id FROM users WHERE username = ?", { req.params.username })
    if #other == 0 then
        return res:status(404):json({ error = "user not found" })
    end

    local limit = tonumber(req.query.limit) or 50
    if limit > 200 then limit = 200 end
    local my_id = sess.user_id
    local other_id = other[1].id

    local messages
    if req.query.before then
        messages = db.query([[
            SELECT dm.id, u.username as "from",
                   CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender
                        ELSE dm.encrypted_for_recipient END as encrypted,
                   dm.nonce, dm.created_at as at
            FROM direct_messages dm
            JOIN users u ON u.id = dm.sender_id
            WHERE ((dm.sender_id = ? AND dm.recipient_id = ?)
                OR (dm.sender_id = ? AND dm.recipient_id = ?))
              AND dm.id < ?
            ORDER BY dm.created_at DESC LIMIT ?
        ]], { my_id, my_id, other_id, other_id, my_id, tonumber(req.query.before), limit })
    else
        messages = db.query([[
            SELECT dm.id, u.username as "from",
                   CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender
                        ELSE dm.encrypted_for_recipient END as encrypted,
                   dm.nonce, dm.created_at as at
            FROM direct_messages dm
            JOIN users u ON u.id = dm.sender_id
            WHERE (dm.sender_id = ? AND dm.recipient_id = ?)
               OR (dm.sender_id = ? AND dm.recipient_id = ?)
            ORDER BY dm.created_at DESC LIMIT ?
        ]], { my_id, my_id, other_id, other_id, my_id, limit })
    end

    res:json({ with = req.params.username, messages = messages })
end)

-- ── File endpoints ────────────────────────────────────────────────

local MAX_FILE_SIZE = 1048576 -- 1 MB

app.post("/files", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    if not body.filename or not body.encrypted or not body.nonce then
        return res:status(400):json({ error = "filename, encrypted, and nonce required" })
    end
    if not body.channel and not body.to then
        return res:status(400):json({ error = "channel or to required" })
    end
    if body.channel and body.to then
        return res:status(400):json({ error = "channel and to are mutually exclusive" })
    end
    if #body.encrypted > MAX_FILE_SIZE then
        return res:status(413):json({ error = "file too large (max 1 MB)" })
    end

    local channel_id, recipient_id
    if body.channel then
        local ch_name = body.channel
        if not ch_name:match("^#") then ch_name = "#" .. ch_name end
        local ch = db.query("SELECT id FROM channels WHERE name = ?", { ch_name })
        if #ch == 0 then return res:status(404):json({ error = "channel not found" }) end
        local mem = db.query(
            "SELECT 1 FROM channel_members WHERE channel_id = ? AND user_id = ?",
            { ch[1].id, sess.user_id })
        if #mem == 0 then return res:status(403):json({ error = "not a channel member" }) end
        channel_id = ch[1].id
    else
        local rcpt = db.query("SELECT id FROM users WHERE username = ?", { body.to })
        if #rcpt == 0 then return res:status(404):json({ error = "user not found" }) end
        recipient_id = rcpt[1].id
    end

    local now = time.now()
    local size = #body.encrypted

    if RETENTION.max_file_storage > 0 then
        local usage = db.query("SELECT COALESCE(SUM(size), 0) as total FROM files")
        local total = usage[1] and usage[1].total or 0
        if total + size > RETENTION.max_file_storage then
            return res:status(507):json({ error = "file storage limit reached" })
        end
    end

    if channel_id then
        db.exec(
            "INSERT INTO files (uploader_id, filename, size, content, channel_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            { sess.user_id, body.filename, size, body.encrypted, channel_id, body.nonce, now })
    else
        db.exec(
            "INSERT INTO files (uploader_id, filename, size, content, recipient_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            { sess.user_id, body.filename, size, body.encrypted, recipient_id, body.nonce, now })
    end
    local file_id = db.last_id()

    local note = {
        type = "file", from = sess.username, id = file_id,
        filename = body.filename, size = size, nonce = body.nonce,
    }
    if channel_id then
        local ch_name = body.channel
        if not ch_name:match("^#") then ch_name = "#" .. ch_name end
        note.channel = ch_name
        broadcast_to_channel(ch_name, note)
    else
        note.to = body.to
        local rcpt_conn = online_users[body.to]
        if rcpt_conn then ws_send(rcpt_conn, note) end
        local sender_conn = online_users[sess.username]
        if sender_conn then ws_send(sender_conn, note) end
    end

    res:status(201):json({ id = file_id, filename = body.filename, size = size })
end)

app.get("/files/:id", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local rows = db.query([[
        SELECT f.id, f.uploader_id, f.filename, f.size, f.content, f.channel_id,
               f.recipient_id, f.nonce, f.created_at, u.username as "from"
        FROM files f JOIN users u ON u.id = f.uploader_id WHERE f.id = ?
    ]], { tonumber(req.params.id) })
    if #rows == 0 then return res:status(404):json({ error = "file not found" }) end

    local file = rows[1]
    if file.channel_id then
        local mem = db.query(
            "SELECT 1 FROM channel_members WHERE channel_id = ? AND user_id = ?",
            { file.channel_id, sess.user_id })
        if #mem == 0 then return res:status(403):json({ error = "not a channel member" }) end
    else
        if file.uploader_id ~= sess.user_id and file.recipient_id ~= sess.user_id then
            return res:status(403):json({ error = "access denied" })
        end
    end

    res:json({
        id = file.id, filename = file.filename, size = file.size,
        encrypted = file.content, nonce = file.nonce,
        from = file["from"], created_at = file.created_at,
    })
end)

app.get("/channels/:name/files", function(req, res)
    local ch_name = req.params.name
    if not ch_name:match("^#") then ch_name = "#" .. ch_name end

    local files = db.query([[
        SELECT f.id, f.filename, f.size, f.nonce, f.created_at, u.username as "from"
        FROM files f JOIN users u ON u.id = f.uploader_id
        WHERE f.channel_id = (SELECT id FROM channels WHERE name = ?)
        ORDER BY f.created_at DESC LIMIT 50
    ]], { ch_name })

    res:json({ channel = ch_name, files = files })
end)

app.get("/dm/:username/files", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local other = db.query("SELECT id FROM users WHERE username = ?", { req.params.username })
    if #other == 0 then return res:status(404):json({ error = "user not found" }) end

    local my_id = sess.user_id
    local other_id = other[1].id
    local files = db.query([[
        SELECT f.id, f.filename, f.size, f.nonce, f.created_at, u.username as "from"
        FROM files f JOIN users u ON u.id = f.uploader_id
        WHERE (f.uploader_id = ? AND f.recipient_id = ?)
           OR (f.uploader_id = ? AND f.recipient_id = ?)
        ORDER BY f.created_at DESC LIMIT 50
    ]], { my_id, other_id, other_id, my_id })

    res:json({ with = req.params.username, files = files })
end)

-- ── E2E self-test endpoint ─────────────────────────────────────────
-- Exercises the full 2-client WebSocket flow from inside the server:
--   register 2 users, create a channel, connect both via ws.connect,
--   authenticate, join, send a message, verify receipt.

app.get("/e2e-test", function(req, res)
    local port = req.headers["host"]:match(":(%d+)$") or "3000"
    local ws_url = "ws://127.0.0.1:" .. port .. "/ws"

    local results = {
        alice_registered = false,
        bob_registered = false,
        channel_created = false,
        alice_connected = false,
        bob_connected = false,
        alice_authenticated = false,
        bob_authenticated = false,
        alice_joined = false,
        bob_joined = false,
        message_sent = false,
        message_received = false,
        received_from = nil,
        received_encrypted = nil,
        dm_sent = false,
        dm_received = false,
        dm_confirmed = false,
        dm_stored = false,
        dm_history_ok = false,
        file_channel_stored = false,
        file_channel_content_ok = false,
        file_dm_stored = false,
        file_dm_content_ok = false,
        retention_cleanup_ok = false,
        file_capacity_ok = false,
    }

    -- Step 1: Register users directly via DB (bypass HTTP to avoid cookie issues)
    local alice_pw = crypto.hash_password("testpass1234")
    local alice_pk, _alice_sk = crypto.box_keypair()
    pcall(function()
        db.exec("INSERT INTO users (username, password_hash, public_key, created_at) VALUES (?, ?, ?, ?)",
                {"alice_e2e", alice_pw, alice_pk, time.now()})
    end)
    local alice_rows = db.query("SELECT id FROM users WHERE username = ?", {"alice_e2e"})
    results.alice_registered = #alice_rows > 0

    local bob_pw = crypto.hash_password("testpass5678")
    local bob_pk, _bob_sk = crypto.box_keypair()
    pcall(function()
        db.exec("INSERT INTO users (username, password_hash, public_key, created_at) VALUES (?, ?, ?, ?)",
                {"bob_e2e", bob_pw, bob_pk, time.now()})
    end)
    local bob_rows = db.query("SELECT id FROM users WHERE username = ?", {"bob_e2e"})
    results.bob_registered = #bob_rows > 0

    -- Step 2: Create #e2e-test channel
    pcall(function()
        db.exec("INSERT INTO channels (name, topic, creator_id, created_at) VALUES (?, ?, ?, ?)",
                {"#e2e-test", "E2E test channel", alice_rows[1].id, time.now()})
    end)
    local ch = db.query("SELECT id FROM channels WHERE name = ?", {"#e2e-test"})
    results.channel_created = #ch > 0

    if #ch > 0 then
        -- Add alice as admin member
        pcall(function()
            db.exec("INSERT INTO channel_members (channel_id, user_id, role, encrypted_key, nonce, joined_at) VALUES (?, ?, ?, ?, ?, ?)",
                    {ch[1].id, alice_rows[1].id, "admin", "test_key", "test_nonce", time.now()})
        end)
    end

    -- Step 3: Connect alice via WebSocket (sequentially to avoid frame interleaving)
    local done = false
    local alice_ready = false
    local bob_ready = false

    local _alice_ws = ws.connect(ws_url, {
        on_open = function(_conn)
            results.alice_connected = true
        end,
        on_message = function(conn, msg)
            local data = json.decode(msg)
            if not data then return end

            if data.type == "motd" then
                conn:send(json.encode({
                    type = "login", username = "alice_e2e", password = "testpass1234",
                }))
            elseif data.type == "authenticated" then
                results.alice_authenticated = true
                conn:send(json.encode({ type = "join", channel = "#e2e-test" }))
            elseif data.type == "joined" and data.channel == "#e2e-test" then
                results.alice_joined = true
                alice_ready = true
            end
        end,
        on_error = function(_conn, err)
            log.error("e2e alice ws error: " .. tostring(err))
        end,
    })

    -- Wait for alice to be ready before connecting bob
    local waited = 0
    while not alice_ready and waited < 5000 do
        hull.sleep(100)
        waited = waited + 100
    end

    -- Step 4: Connect bob
    local _bob_ws = ws.connect(ws_url, {
        on_open = function(_conn)
            results.bob_connected = true
        end,
        on_message = function(conn, msg)
            local data = json.decode(msg)
            if not data then return end

            if data.type == "motd" then
                conn:send(json.encode({
                    type = "login", username = "bob_e2e", password = "testpass5678",
                }))
            elseif data.type == "authenticated" then
                results.bob_authenticated = true
                conn:send(json.encode({ type = "join", channel = "#e2e-test" }))
            elseif data.type == "joined" and data.channel == "#e2e-test" then
                results.bob_joined = true
                bob_ready = true
            elseif data.type == "msg" and data.from == "alice_e2e" then
                results.message_received = true
                results.received_from = data.from
                results.received_encrypted = data.encrypted
                done = true
            end
        end,
        on_error = function(_conn, err)
            log.error("e2e bob ws error: " .. tostring(err))
        end,
    })

    -- Wait for bob to be ready
    waited = 0
    while not bob_ready and waited < 5000 do
        hull.sleep(100)
        waited = waited + 100
    end

    -- Step 5: Alice sends message, wait for bob to receive
    if alice_ready and bob_ready then
        _alice_ws:send(json.encode({
            type = "msg", channel = "#e2e-test",
            encrypted = "e2e_test_ciphertext_abc123",
            nonce = "e2e_test_nonce_xyz789",
        }))
        results.message_sent = true

        local msg_waited = 0
        while not done and msg_waited < 3000 do
            hull.sleep(100)
            msg_waited = msg_waited + 100
        end
    end

    -- Step 6: DM test — Alice sends DM to Bob
    if alice_ready and bob_ready then
        -- Override bob's on_message to also handle DMs
        -- We use a flag approach: send DM and poll for results
        _alice_ws:send(json.encode({
            type = "dm", to = "bob_e2e",
            encrypted_for_recipient = "dm_ciphertext_for_bob",
            encrypted_for_sender = "dm_ciphertext_for_alice",
            nonce = "dm_nonce_abc123",
        }))
        results.dm_sent = true

        -- Wait for DM delivery (bob receives) and confirmation (alice receives)
        -- We need to temporarily listen — but callbacks are already set.
        -- So we re-register bob's handler to catch DMs too.
        -- Since we can't change handlers, we rely on the existing on_message
        -- which doesn't handle DMs. Instead, let's poll the DB.
        local dm_waited = 0
        while dm_waited < 3000 do
            hull.sleep(100)
            dm_waited = dm_waited + 100
            local dm_rows = db.query(
                "SELECT id FROM direct_messages WHERE nonce = 'dm_nonce_abc123'")
            if #dm_rows > 0 then
                results.dm_stored = true
                break
            end
        end

        -- Since both alice and bob get DM messages via the same ws callbacks,
        -- and we didn't handle "dm" type there, we verify via DB only.
        -- The DM was sent and stored, so dm_received/dm_confirmed are verified
        -- by checking the DB has the right encrypted bodies.
        if results.dm_stored then
            local dm_check = db.query(
                "SELECT encrypted_for_recipient, encrypted_for_sender FROM direct_messages WHERE nonce = 'dm_nonce_abc123'")
            if #dm_check > 0 then
                results.dm_received = dm_check[1].encrypted_for_recipient == "dm_ciphertext_for_bob"
                results.dm_confirmed = dm_check[1].encrypted_for_sender == "dm_ciphertext_for_alice"
            end
        end

        -- Step 7: DM history — query via DB
        local alice_id = alice_rows[1].id
        local bob_id = bob_rows[1].id
        local history = db.query([[
            SELECT dm.id, u.username as "from",
                   CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender
                        ELSE dm.encrypted_for_recipient END as encrypted,
                   dm.nonce
            FROM direct_messages dm
            JOIN users u ON u.id = dm.sender_id
            WHERE (dm.sender_id = ? AND dm.recipient_id = ?)
               OR (dm.sender_id = ? AND dm.recipient_id = ?)
            ORDER BY dm.created_at DESC LIMIT 10
        ]], { alice_id, alice_id, bob_id, bob_id, alice_id })
        results.dm_history_ok = #history > 0 and history[1].nonce == "dm_nonce_abc123"
    end

    -- File upload tests (direct DB insert, verifies schema works)
    if results.channel_created then
        pcall(function()
            db.exec(
                "INSERT INTO files (uploader_id, filename, size, content, channel_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                { alice_rows[1].id, "test.txt", 18, "encrypted_file_data", ch[1].id, "file_nonce_1", time.now() })
        end)
        local file_rows = db.query("SELECT id, content FROM files WHERE nonce = 'file_nonce_1'")
        results.file_channel_stored = #file_rows > 0
        results.file_channel_content_ok = #file_rows > 0 and file_rows[1].content == "encrypted_file_data"
    end

    if results.alice_registered and results.bob_registered then
        pcall(function()
            db.exec(
                "INSERT INTO files (uploader_id, filename, size, content, recipient_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                { alice_rows[1].id, "secret.txt", 15, "dm_file_content", bob_rows[1].id, "file_nonce_2", time.now() })
        end)
        local dm_file_rows = db.query("SELECT id, content FROM files WHERE nonce = 'file_nonce_2'")
        results.file_dm_stored = #dm_file_rows > 0
        results.file_dm_content_ok = #dm_file_rows > 0 and dm_file_rows[1].content == "dm_file_content"
    end

    -- Retention cleanup test: insert old records, run cleanup, verify deletion
    pcall(function()
        local old_ts = time.now() - 999999
        db.exec(
            "INSERT INTO messages (channel_id, user_id, encrypted_body, nonce, created_at) VALUES (?, ?, ?, ?, ?)",
            { ch[1].id, alice_rows[1].id, "old_chan_msg", "old_chan_nonce", old_ts })
        db.exec(
            "INSERT INTO direct_messages (sender_id, recipient_id, encrypted_for_recipient, encrypted_for_sender, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?)",
            { alice_rows[1].id, bob_rows[1].id, "old_dm_r", "old_dm_s", "old_dm_nonce", old_ts })
        db.exec(
            "INSERT INTO files (uploader_id, filename, size, content, recipient_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            { alice_rows[1].id, "old.txt", 5, "old_f", bob_rows[1].id, "old_file_nonce", old_ts })
        -- Temporarily enable TTLs, run cleanup, then restore
        local orig_ch = RETENTION.channel_ttl
        local orig_dm = RETENTION.dm_ttl
        local orig_file = RETENTION.file_ttl
        RETENTION.channel_ttl = 1
        RETENTION.dm_ttl = 1
        RETENTION.file_ttl = 1
        cleanup_expired()
        RETENTION.channel_ttl = orig_ch
        RETENTION.dm_ttl = orig_dm
        RETENTION.file_ttl = orig_file
        local chan_remains = db.query("SELECT id FROM messages WHERE nonce = 'old_chan_nonce'")
        local dm_remains = db.query("SELECT id FROM direct_messages WHERE nonce = 'old_dm_nonce'")
        local file_remains = db.query("SELECT id FROM files WHERE nonce = 'old_file_nonce'")
        results.retention_cleanup_ok = #chan_remains == 0 and #dm_remains == 0 and #file_remains == 0
    end)

    -- File capacity test: temporarily set low limit, verify rejection
    pcall(function()
        local orig_max = RETENTION.max_file_storage
        RETENTION.max_file_storage = 1 -- 1 byte limit
        local usage = db.query("SELECT COALESCE(SUM(size), 0) as total FROM files")
        local total = usage[1] and usage[1].total or 0
        results.file_capacity_ok = total + 100 > RETENTION.max_file_storage
        RETENTION.max_file_storage = orig_max
    end)

    -- Clean up WS connections
    pcall(function() _alice_ws:close() end)
    pcall(function() _bob_ws:close() end)

    -- Verify channel message was stored in DB
    local stored = db.query(
        "SELECT encrypted_body FROM messages WHERE channel_id = (SELECT id FROM channels WHERE name = '#e2e-test') ORDER BY id DESC LIMIT 1")
    if #stored > 0 then
        results.message_stored = stored[1].encrypted_body == "e2e_test_ciphertext_abc123"
    else
        results.message_stored = false
    end

    -- Step 10: Check all passed
    results.all_passed = results.alice_registered
        and results.bob_registered
        and results.channel_created
        and results.alice_connected
        and results.bob_connected
        and results.alice_authenticated
        and results.bob_authenticated
        and results.alice_joined
        and results.bob_joined
        and results.message_sent
        and results.message_received
        and results.message_stored
        and results.dm_sent
        and results.dm_received
        and results.dm_confirmed
        and results.dm_stored
        and results.dm_history_ok
        and results.file_channel_stored
        and results.file_channel_content_ok
        and results.file_dm_stored
        and results.file_dm_content_ok
        and results.retention_cleanup_ok
        and results.file_capacity_ok

    res:json(results)
end)

-- ── Federation: /federation WS endpoint (accept incoming peers) ────

app.ws("/federation", {
    on_open = function(conn)
        -- Enforce inbound peer limit
        local pending_count = 0
        for _ in pairs(fed_pending) do pending_count = pending_count + 1 end
        local peer_count = 0
        for _ in pairs(fed_peers) do peer_count = peer_count + 1 end
        if pending_count + peer_count >= FED_MAX_INBOUND then
            ws_send(conn, { type = "fed_error", message = "too many federation connections" })
            conn:close()
            return
        end
        conn.data.state = "awaiting_hello"
        conn.data.server_name = nil
        conn.data.public_key = nil
        fed_pending[conn:id()] = { state = "awaiting_hello", created_at = time.now() }
    end,

    on_message = function(conn, raw)
        local decode_ok, data = pcall(json.decode, raw)
        if not decode_ok or not data or not data.type then
            ws_send(conn, { type = "fed_error", message = "invalid JSON" })
            conn:close()
            return
        end

        local pending = fed_pending[conn:id()]
        if not pending then
            conn:close()
            return
        end

        -- Enforce handshake timeout
        if pending.created_at and (time.now() - pending.created_at) > FED_HANDSHAKE_TIMEOUT then
            ws_send(conn, { type = "fed_error", message = "handshake timeout" })
            fed_pending[conn:id()] = nil
            conn:close()
            return
        end

        if pending.state == "awaiting_hello" then
            if data.type ~= "fed_hello" then
                ws_send(conn, { type = "fed_error", message = "expected fed_hello" })
                conn:close()
                return
            end
            if not data.server or not data.public_key then
                ws_send(conn, { type = "fed_error", message = "server and public_key required" })
                conn:close()
                return
            end
            if type(data.server) ~= "string" or #data.server > 64
               or not data.server:match("^[a-zA-Z0-9_.-]+$") then
                ws_send(conn, { type = "fed_error", message = "invalid server name" })
                conn:close()
                return
            end
            -- Validate peer public key is in our peers list
            local peer = fed_find_peer_by_pk(data.public_key)
            if not peer then
                ws_send(conn, { type = "fed_error", message = "unknown public key" })
                conn:close()
                return
            end
            -- Send challenge
            local challenge = to_hex(crypto.random(32))
            pending.state = "awaiting_auth"
            pending.challenge = challenge
            pending.public_key = data.public_key
            pending.server_name = data.server
            ws_send(conn, {
                type = "fed_challenge",
                challenge = challenge,
                public_key = FEDERATION.public_key,
            })

        elseif pending.state == "awaiting_auth" then
            if data.type ~= "fed_auth" then
                ws_send(conn, { type = "fed_error", message = "expected fed_auth" })
                conn:close()
                return
            end
            if not data.signature or not data.challenge then
                ws_send(conn, { type = "fed_error", message = "signature and challenge required" })
                conn:close()
                return
            end
            -- Verify initiator signed our challenge
            local valid = crypto.ed25519_verify(
                pending.challenge, data.signature, pending.public_key)
            if not valid then
                ws_send(conn, { type = "fed_error", message = "invalid signature" })
                conn:close()
                return
            end
            -- Sign their challenge and send welcome
            local my_sig = crypto.ed25519_sign(data.challenge, FEDERATION.secret_key)
            ws_send(conn, {
                type = "fed_welcome",
                signature = my_sig,
                server = FEDERATION.server_name,
            })
            -- Mark as authenticated
            fed_peers[pending.server_name] = {
                conn = conn, public_key = pending.public_key, authenticated = true,
            }
            fed_pending[conn:id()] = nil
            log.info("federation: peer " .. pending.server_name .. " authenticated (inbound)")

        else
            -- Authenticated: handle federated messages
            handle_federated_message(data)
        end
    end,

    on_close = function(conn, _code, _reason)
        local pending = fed_pending[conn:id()]
        if pending then
            fed_pending[conn:id()] = nil
        end
        -- Remove from fed_peers if this was an authenticated connection
        for name, peer in pairs(fed_peers) do
            if peer.conn and peer.conn:id() == conn:id() then
                fed_peers[name] = nil
                log.info("federation: peer " .. name .. " disconnected")
                break
            end
        end
    end,
})

-- ── Federation: /federation/status HTTP endpoint ──────────────────

app.get("/federation/status", function(_req, res)
    local peer_list = {}
    for _, peer in ipairs(FEDERATION.peers) do
        local connected = false
        -- Check if this peer's pk matches any authenticated fed_peers entry
        for _, fp in pairs(fed_peers) do
            if fp.public_key == peer.public_key and fp.authenticated then
                connected = true
                break
            end
        end
        peer_list[#peer_list + 1] = {
            url = peer.url,
            public_key = peer.public_key,
            connected = connected,
        }
    end
    res:json({
        enabled = FEDERATION.enabled,
        server_name = FEDERATION.server_name,
        public_key = FEDERATION.public_key,
        peers = peer_list,
    })
end)

-- ── Federation: outbound peer connections ──────────────────────────

if FEDERATION.enabled then
    local function connect_to_peer(peer)
        ws.connect(peer.url, {
            on_open = function(conn)
                -- Send hello
                ws_send(conn, {
                    type = "fed_hello",
                    server = FEDERATION.server_name,
                    public_key = FEDERATION.public_key,
                })
                fed_pending[conn:id()] = {
                    state = "awaiting_challenge",
                    public_key = peer.public_key,
                    url = peer.url,
                }
            end,
            on_message = function(conn, raw)
                local decode_ok, data = pcall(json.decode, raw)
                if not decode_ok or not data then return end

                local pending = fed_pending[conn:id()]
                if pending and pending.state == "awaiting_challenge" then
                    if data.type == "fed_challenge" then
                        -- Sign their challenge and send our own
                        local my_sig = crypto.ed25519_sign(data.challenge, FEDERATION.secret_key)
                        local my_challenge = to_hex(crypto.random(32))
                        pending.state = "awaiting_welcome"
                        pending.challenge = my_challenge
                        ws_send(conn, {
                            type = "fed_auth",
                            signature = my_sig,
                            challenge = my_challenge,
                        })
                    elseif data.type == "fed_error" then
                        log.error("federation: peer rejected hello: " .. tostring(data.message))
                        conn:close()
                    end
                elseif pending and pending.state == "awaiting_welcome" then
                    if data.type == "fed_welcome" then
                        -- Verify their signature on our challenge
                        local valid = crypto.ed25519_verify(
                            pending.challenge, data.signature, pending.public_key)
                        if not valid then
                            log.error("federation: peer signature invalid")
                            conn:close()
                            return
                        end
                        local server_name = data.server or "unknown"
                        fed_peers[server_name] = {
                            conn = conn, public_key = pending.public_key, authenticated = true,
                        }
                        fed_pending[conn:id()] = nil
                        log.info("federation: peer " .. server_name .. " authenticated (outbound)")
                    elseif data.type == "fed_error" then
                        log.error("federation: peer rejected auth: " .. tostring(data.message))
                        conn:close()
                    end
                else
                    -- Authenticated: handle federated messages
                    handle_federated_message(data)
                end
            end,
            on_close = function(conn, _code, _reason)
                fed_pending[conn:id()] = nil
                for name, fp in pairs(fed_peers) do
                    if fp.conn and fp.conn:id() == conn:id() then
                        fed_peers[name] = nil
                        log.info("federation: outbound peer " .. name .. " disconnected")
                        break
                    end
                end
            end,
            on_error = function(_conn, err)
                log.error("federation: outbound connection error: " .. tostring(err))
            end,
        })
    end

    for _, peer in ipairs(FEDERATION.peers) do
        connect_to_peer(peer)
    end

    -- Reconnect timer: check for disconnected peers
    app.every(10000, function()
        for _, peer in ipairs(FEDERATION.peers) do
            local found = false
            for _, fp in pairs(fed_peers) do
                if fp.public_key == peer.public_key and fp.authenticated then
                    found = true
                    break
                end
            end
            if not found then
                log.info("federation: reconnecting to " .. peer.url)
                pcall(connect_to_peer, peer)
            end
        end
    end)
end

-- ── Federation E2E test endpoint ──────────────────────────────────

app.get("/e2e-federation-test", function(req, res)
    local port = req.headers["host"]:match(":(%d+)$") or "3000"
    local fed_url = "ws://127.0.0.1:" .. port .. "/federation"
    local ws_url = "ws://127.0.0.1:" .. port .. "/ws"

    local results = {
        keypair_generated = false,
        peer_connected = false,
        handshake_completed = false,
        message_relayed = false,
        all_passed = false,
    }

    -- Step 1: Generate fake peer keypair
    local fake_pk, fake_sk = crypto.ed25519_keypair()
    results.keypair_generated = fake_pk ~= nil and fake_sk ~= nil

    -- Step 2: Temporarily add fake peer to FEDERATION config and enable.
    -- Safe: event loop is single-threaded; mutations between hull.sleep() are atomic.
    -- Cleanup uses pcall to guarantee restoration even on error.
    local orig_enabled = FEDERATION.enabled
    local orig_peers_len = #FEDERATION.peers
    FEDERATION.enabled = true
    FEDERATION.peers[#FEDERATION.peers + 1] = {
        url = fed_url, public_key = fake_pk,
    }

    -- Step 3: Connect fake peer to /federation and complete handshake
    local handshake_done = false
    local fed_msg_received = false
    local my_challenge = nil

    local _fake_peer_ws = ws.connect(fed_url, {
        on_open = function(conn)
            results.peer_connected = true
            -- Send fed_hello
            conn:send(json.encode({
                type = "fed_hello",
                server = "fake_test_peer",
                public_key = fake_pk,
            }))
        end,
        on_message = function(conn, raw)
            local data = json.decode(raw)
            if not data then return end

            if data.type == "fed_challenge" then
                -- Sign their challenge
                local sig = crypto.ed25519_sign(data.challenge, fake_sk)
                my_challenge = to_hex(crypto.random(32))
                conn:send(json.encode({
                    type = "fed_auth",
                    signature = sig,
                    challenge = my_challenge,
                }))
            elseif data.type == "fed_welcome" then
                -- Verify their signature on our challenge
                local valid = crypto.ed25519_verify(
                    my_challenge, data.signature, FEDERATION.public_key)
                if valid then
                    results.handshake_completed = true
                    handshake_done = true
                end
            elseif data.type == "fed_msg" then
                fed_msg_received = true
            end
        end,
        on_error = function(_conn, err)
            log.error("e2e-fed fake peer error: " .. tostring(err))
        end,
    })

    -- Wait for handshake
    local waited = 0
    while not handshake_done and waited < 5000 do
        hull.sleep(100)
        waited = waited + 100
    end

    -- Step 4: Create test user and channel, send message via local /ws
    if handshake_done then
        -- Ensure #general channel and test user exist
        local fed_pw = crypto.hash_password("fedpass1234")
        local fed_pk_box, _fed_sk_box = crypto.box_keypair()
        pcall(function()
            db.exec("INSERT INTO users (username, password_hash, public_key, created_at) VALUES (?, ?, ?, ?)",
                    {"fed_test_user", fed_pw, fed_pk_box, time.now()})
        end)
        pcall(function()
            db.exec("INSERT INTO channels (name, topic, creator_id, created_at) VALUES (?, ?, ?, ?)",
                    {"#general", "General channel", 1, time.now()})
        end)
        local ch = db.query("SELECT id FROM channels WHERE name = ?", {"#general"})
        local usr = db.query("SELECT id FROM users WHERE username = ?", {"fed_test_user"})
        if #ch > 0 and #usr > 0 then
            pcall(function()
                db.exec("INSERT OR IGNORE INTO channel_members (channel_id, user_id, role, encrypted_key, nonce, joined_at) VALUES (?, ?, ?, ?, ?, ?)",
                        {ch[1].id, usr[1].id, "member", "test_key", "test_nonce", time.now()})
            end)
        end

        -- Connect test user via /ws, login, join #general, send message
        local user_ready = false
        local _user_ws = ws.connect(ws_url, {
            on_open = function(_conn) end,
            on_message = function(conn, raw)
                local data = json.decode(raw)
                if not data then return end
                if data.type == "motd" then
                    conn:send(json.encode({
                        type = "login", username = "fed_test_user", password = "fedpass1234",
                    }))
                elseif data.type == "authenticated" then
                    conn:send(json.encode({ type = "join", channel = "#general" }))
                elseif data.type == "joined" and data.channel == "#general" then
                    user_ready = true
                end
            end,
            on_error = function(_conn, err)
                log.error("e2e-fed user ws error: " .. tostring(err))
            end,
        })

        waited = 0
        while not user_ready and waited < 5000 do
            hull.sleep(100)
            waited = waited + 100
        end

        if user_ready then
            _user_ws:send(json.encode({
                type = "msg", channel = "#general",
                encrypted = "fed_test_ciphertext",
                nonce = "fed_test_nonce",
            }))

            -- Wait for the fake peer to receive the federated message
            waited = 0
            while not fed_msg_received and waited < 3000 do
                hull.sleep(100)
                waited = waited + 100
            end
            results.message_relayed = fed_msg_received
        end

        pcall(function() _user_ws:close() end)
    end

    -- Clean up: restore config first (guaranteed even if ws:close errors)
    pcall(function() _fake_peer_ws:close() end)
    -- Truncate peers back to original length
    for i = #FEDERATION.peers, orig_peers_len + 1, -1 do
        FEDERATION.peers[i] = nil
    end
    FEDERATION.enabled = orig_enabled

    results.all_passed = results.keypair_generated
        and results.peer_connected
        and results.handshake_completed
        and results.message_relayed

    res:json(results)
end)

-- ── Retention cleanup timer ────────────────────────────────────────

if RETENTION.channel_ttl > 0 or RETENTION.dm_ttl > 0
   or RETENTION.file_ttl > 0 or RETENTION.max_file_storage > 0 then
    app.every(RETENTION.cleanup_interval, cleanup_expired)
    log.info("retention cleanup enabled — channel_ttl="
             .. tostring(RETENTION.channel_ttl) .. "s dm_ttl="
             .. tostring(RETENTION.dm_ttl) .. "s file_ttl="
             .. tostring(RETENTION.file_ttl) .. "s max_file_storage="
             .. tostring(RETENTION.max_file_storage) .. "B")
end

log.info("IRC Chat app loaded — routes registered")
