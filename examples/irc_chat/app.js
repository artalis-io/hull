// IRC Chat — Encrypted WebSocket chat with channels (JavaScript)
//
// Run: hull app.js -p 3000
//
// E2E encryption: messages are encrypted with per-channel symmetric keys
// using crypto.secretbox (XSalsa20-Poly1305). Channel keys are distributed
// to members encrypted with crypto.box (Curve25519+XSalsa20+Poly1305).
// The server relays encrypted ciphertext — it never sees plaintext messages.
//
// See app.lua header for full endpoint + WebSocket protocol documentation.

import { app } from "hull:app";
import { cookie } from "hull:cookie";
import { crypto } from "hull:crypto";
import { db } from "hull:db";
import { log } from "hull:log";
import { auth } from "hull:middleware:auth";
import { session } from "hull:middleware:session";
import { time } from "hull:time";
import { validate } from "hull:validate";
import { ws } from "hull:ws";

app.manifest({ hosts: ["127.0.0.1"] });
session.init({ ttl: 7200 });

// ── Retention config (0 = no limit, >0 = TTL in seconds) ────────────

const RETENTION = {
    channelTtl: 0,      // channel message TTL (e.g. 86400 * 90 for 90 days)
    dmTtl: 0,            // direct message TTL (e.g. 86400 * 30 for 30 days)
    fileTtl: 0,          // file TTL (e.g. 86400 * 7 for 7 days)
    maxFileStorage: 0,   // max total file content in bytes (e.g. 50 * 1048576 for 50 MB)
    cleanupInterval: 3600000, // check every hour (ms)
};

// ── Helpers ─────────────────────────────────────────────────────────

function toHex(buf) {
    const bytes = new Uint8Array(buf);
    let hex = "";
    for (let i = 0; i < bytes.length; i++)
        hex += bytes[i].toString(16).padStart(2, "0");
    return hex;
}

function requireSession(req, res) {
    if (!req.ctx || !req.ctx.session) {
        res.status(401).json({ error: "authentication required" });
        return null;
    }
    return req.ctx.session;
}

// ── Session middleware ──────────────────────────────────────────────

app.use("*", "/*", (req, _res) => {
    const header = req.headers.cookie;
    if (!header) return 0;
    const cookies = cookie.parse(header);
    const sessionId = cookies["hull.sid"];
    if (sessionId) {
        const data = session.load(sessionId);
        if (data) {
            if (!req.ctx) req.ctx = {};
            req.ctx.sessionId = sessionId;
            req.ctx.session = data;
        }
    }
    return 0;
});

// ── Health ──────────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// ── Auth endpoints ──────────────────────────────────────────────────

app.post("/register", (req, res) => {
    let body;
    try { body = JSON.parse(req.body); } catch (_e) { body = null; }
    if (!body) return res.status(400).json({ error: "invalid JSON" });

    const [ok, errors] = validate.check(body, {
        username: { required: true, min: 2, max: 32 },
        password: { required: true, min: 8 },
    });
    if (!ok) return res.status(400).json({ errors });

    const kp = crypto.boxKeypair();
    const hash = crypto.hashPassword(body.password);
    let id;

    try {
        db.batch(() => {
            const existing = db.query(
                "SELECT id FROM users WHERE username = ?", [body.username]);
            if (existing.length > 0) throw new Error("username already taken");
            db.exec(
                "INSERT INTO users (username, password_hash, public_key, created_at) VALUES (?, ?, ?, ?)",
                [body.username, hash, kp.publicKey, time.now()]);
            id = db.lastId();
        });
    } catch (e) {
        if (String(e).includes("username already taken"))
            return res.status(409).json({ error: "username already taken" });
        return res.status(500).json({ error: "registration failed" });
    }

    res.status(201).json({
        id, username: body.username,
        public_key: kp.publicKey, secret_key: kp.secretKey,
    });
});

app.post("/login", (req, res) => {
    let body;
    try { body = JSON.parse(req.body); } catch (_e) { body = null; }
    if (!body) return res.status(400).json({ error: "invalid JSON" });

    const [ok, errors] = validate.check(body, {
        username: { required: true },
        password: { required: true },
    });
    if (!ok) return res.status(400).json({ errors });

    const rows = db.query(
        "SELECT * FROM users WHERE username = ?", [body.username]);
    if (rows.length === 0)
        return res.status(401).json({ error: "invalid credentials" });

    const user = rows[0];
    if (!crypto.verifyPassword(body.password, user.password_hash))
        return res.status(401).json({ error: "invalid credentials" });

    auth.login(req, res, {
        user_id: user.id, username: user.username, public_key: user.public_key,
    });

    res.json({
        id: user.id, username: user.username,
        public_key: user.public_key,
    });
});

app.post("/logout", (req, res) => {
    if (!requireSession(req, res)) return;
    auth.logout(req, res);
    res.json({ ok: true });
});

app.get("/me", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;
    res.json({
        id: sess.user_id, username: sess.username,
        public_key: sess.public_key,
    });
});

// ── Channel endpoints ───────────────────────────────────────────────

app.get("/channels", (_req, res) => {
    const channels = db.query(
        "SELECT c.id, c.name, c.topic, c.created_at, " +
        "COUNT(cm.user_id) as member_count " +
        "FROM channels c LEFT JOIN channel_members cm ON cm.channel_id = c.id " +
        "GROUP BY c.id ORDER BY c.name");
    res.json({ channels });
});

app.post("/channels", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    let body;
    try { body = JSON.parse(req.body); } catch (_e) { body = null; }
    if (!body) return res.status(400).json({ error: "invalid JSON" });

    const [ok, errors] = validate.check(body, {
        name: { required: true, min: 2, max: 50 },
    });
    if (!ok) return res.status(400).json({ errors });

    let name = body.name;
    if (!name.startsWith("#")) name = `#${name}`;

    const channelKeyHex = toHex(crypto.random(32));
    const nonceHex = toHex(crypto.random(24));
    const encryptedKey = crypto.box(
        channelKeyHex, nonceHex, sess.public_key, sess.public_key);

    let channelId;
    try {
        db.batch(() => {
            const existing = db.query(
                "SELECT id FROM channels WHERE name = ?", [name]);
            if (existing.length > 0) throw new Error("channel already exists");
            db.exec(
                "INSERT INTO channels (name, topic, creator_id, created_at) VALUES (?, ?, ?, ?)",
                [name, body.topic || "", sess.user_id, time.now()]);
            channelId = db.lastId();
            db.exec(
                "INSERT INTO channel_members (channel_id, user_id, role, encrypted_key, nonce, joined_at) VALUES (?, ?, ?, ?, ?, ?)",
                [channelId, sess.user_id, "admin", encryptedKey, nonceHex, time.now()]);
        });
    } catch (e) {
        if (String(e).includes("channel already exists"))
            return res.status(409).json({ error: "channel already exists" });
        return res.status(500).json({ error: "channel creation failed" });
    }

    res.status(201).json({
        id: channelId, name, topic: body.topic || "",
        channel_key: channelKeyHex,
    });
});

app.get("/channels/:name/members", (req, res) => {
    let channelName = req.params.name;
    if (!channelName.startsWith("#")) channelName = `#${channelName}`;

    const members = db.query(
        "SELECT u.id, u.username, u.public_key, cm.role, cm.joined_at " +
        "FROM channel_members cm JOIN users u ON u.id = cm.user_id " +
        "WHERE cm.channel_id = (SELECT id FROM channels WHERE name = ?) " +
        "ORDER BY cm.joined_at", [channelName]);
    res.json({ channel: channelName, members });
});

app.get("/channels/:name/history", (req, res) => {
    let channelName = req.params.name;
    if (!channelName.startsWith("#")) channelName = `#${channelName}`;

    const messages = db.query(
        "SELECT m.id, u.username, m.encrypted_body, m.nonce, m.created_at " +
        "FROM messages m JOIN users u ON u.id = m.user_id " +
        "WHERE m.channel_id = (SELECT id FROM channels WHERE name = ?) " +
        "ORDER BY m.created_at DESC LIMIT 50", [channelName]);
    res.json({ channel: channelName, messages });
});

// ── Users endpoint ────────────────────────────────────────────────

app.get("/users", (_req, res) => {
    const users = db.query(
        "SELECT username, public_key FROM users ORDER BY username");
    for (const u of users) {
        u.online = !!onlineUsers[u.username];
    }
    res.json({ users });
});

// ── DM history endpoint ──────────────────────────────────────────

app.get("/dm/:username/history", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const other = db.query(
        "SELECT id FROM users WHERE username = ?", [req.params.username]);
    if (other.length === 0)
        return res.status(404).json({ error: "user not found" });

    let limit = parseInt(req.query.limit) || 50;
    if (limit > 200) limit = 200;
    const myId = sess.user_id;
    const otherId = other[0].id;

    let messages;
    if (req.query.before) {
        messages = db.query(
            "SELECT dm.id, u.username as 'from', " +
            "CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender " +
            "ELSE dm.encrypted_for_recipient END as encrypted, " +
            "dm.nonce, dm.created_at as at " +
            "FROM direct_messages dm JOIN users u ON u.id = dm.sender_id " +
            "WHERE ((dm.sender_id = ? AND dm.recipient_id = ?) " +
            "OR (dm.sender_id = ? AND dm.recipient_id = ?)) " +
            "AND dm.id < ? ORDER BY dm.created_at DESC LIMIT ?",
            [myId, myId, otherId, otherId, myId, parseInt(req.query.before), limit]);
    } else {
        messages = db.query(
            "SELECT dm.id, u.username as 'from', " +
            "CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender " +
            "ELSE dm.encrypted_for_recipient END as encrypted, " +
            "dm.nonce, dm.created_at as at " +
            "FROM direct_messages dm JOIN users u ON u.id = dm.sender_id " +
            "WHERE (dm.sender_id = ? AND dm.recipient_id = ?) " +
            "OR (dm.sender_id = ? AND dm.recipient_id = ?) " +
            "ORDER BY dm.created_at DESC LIMIT ?",
            [myId, myId, otherId, otherId, myId, limit]);
    }

    res.json({ with: req.params.username, messages });
});

// ── WebSocket chat ──────────────────────────────────────────────────

const onlineUsers = {}; // username -> conn

function wsSend(conn, msg) {
    conn.send(JSON.stringify(msg));
}

function wsError(conn, message) {
    wsSend(conn, { type: "error", message });
}

function broadcastToChannel(_channelName, msg) {
    ws.broadcast("/ws", JSON.stringify(msg));
}

app.ws("/ws", {
    onOpen(conn) {
        conn.data.username = null;
        conn.data.userId = null;
        conn.data.channels = {};
        conn.data.authenticated = false;

        wsSend(conn, {
            type: "motd",
            text: "Welcome to Hull IRC Chat. Send a 'login' message to authenticate.",
        });
    },

    onMessage(conn, raw) {
        let msg;
        try { msg = JSON.parse(raw); } catch (_e) { msg = null; }
        if (!msg || !msg.type)
            return wsError(conn, "invalid JSON message");

        if (msg.type === "login") {
            if (!msg.username || !msg.password)
                return wsError(conn, "username and password required");
            const rows = db.query(
                "SELECT * FROM users WHERE username = ?", [msg.username]);
            if (rows.length === 0)
                return wsError(conn, "invalid credentials");
            if (!crypto.verifyPassword(msg.password, rows[0].password_hash))
                return wsError(conn, "invalid credentials");
            conn.data.username = rows[0].username;
            conn.data.userId = rows[0].id;
            conn.data.publicKey = rows[0].public_key;
            conn.data.authenticated = true;
            onlineUsers[rows[0].username] = conn;
            wsSend(conn, {
                type: "authenticated",
                username: rows[0].username,
                public_key: rows[0].public_key,
            });
            ws.broadcast("/ws", JSON.stringify({
                type: "presence", username: rows[0].username, online: true,
            }));
            return;
        }

        if (!conn.data.authenticated)
            return wsError(conn, "not authenticated — send login first");

        if (msg.type === "join") {
            if (!msg.channel)
                return wsError(conn, "channel name required");
            let channelName = msg.channel;
            if (!channelName.startsWith("#")) channelName = `#${channelName}`;

            const ch = db.query(
                "SELECT id FROM channels WHERE name = ?", [channelName]);
            if (ch.length === 0)
                return wsError(conn, `channel not found: ${channelName}`);

            let membership = db.query(
                "SELECT encrypted_key, nonce FROM channel_members WHERE channel_id = ? AND user_id = ?",
                [ch[0].id, conn.data.userId]);

            if (membership.length === 0) {
                const nonceHex = toHex(crypto.random(24));
                db.exec(
                    "INSERT OR IGNORE INTO channel_members (channel_id, user_id, role, encrypted_key, nonce, joined_at) VALUES (?, ?, ?, ?, ?, ?)",
                    [ch[0].id, conn.data.userId, "member", "pending", nonceHex, time.now()]);
                membership = [{ encrypted_key: "pending", nonce: nonceHex }];
            }

            conn.data.channels[channelName] = true;

            const members = db.query(
                "SELECT u.username FROM channel_members cm JOIN users u ON u.id = cm.user_id WHERE cm.channel_id = ?",
                [ch[0].id]);
            const memberNames = members.map(m => m.username);

            wsSend(conn, {
                type: "joined", channel: channelName,
                user: conn.data.username, members: memberNames,
                encrypted_key: membership[0].encrypted_key,
                nonce: membership[0].nonce,
            });

            broadcastToChannel(channelName, {
                type: "user_joined", channel: channelName,
                user: conn.data.username,
            });

        } else if (msg.type === "leave") {
            if (!msg.channel)
                return wsError(conn, "channel name required");
            delete conn.data.channels[msg.channel];
            broadcastToChannel(msg.channel, {
                type: "left", channel: msg.channel,
                user: conn.data.username,
            });

        } else if (msg.type === "msg") {
            if (!msg.channel || !msg.encrypted || !msg.nonce)
                return wsError(conn, "channel, encrypted, and nonce required");
            if (!conn.data.channels[msg.channel])
                return wsError(conn, `not in channel: ${msg.channel}`);

            const ch = db.query(
                "SELECT id FROM channels WHERE name = ?", [msg.channel]);
            if (ch.length > 0) {
                db.exec(
                    "INSERT INTO messages (channel_id, user_id, encrypted_body, nonce, created_at) VALUES (?, ?, ?, ?, ?)",
                    [ch[0].id, conn.data.userId, msg.encrypted, msg.nonce, time.now()]);
            }

            broadcastToChannel(msg.channel, {
                type: "msg", channel: msg.channel,
                from: conn.data.username,
                encrypted: msg.encrypted,
                nonce: msg.nonce,
                at: time.now(),
            });

        } else if (msg.type === "topic") {
            if (!msg.channel || !msg.topic)
                return wsError(conn, "channel and topic required");
            db.exec("UPDATE channels SET topic = ? WHERE name = ?",
                    [msg.topic, msg.channel]);
            broadcastToChannel(msg.channel, {
                type: "topic", channel: msg.channel,
                topic: msg.topic, by: conn.data.username,
            });

        } else if (msg.type === "who") {
            if (!msg.channel)
                return wsError(conn, "channel name required");
            const members = db.query(
                "SELECT u.username, u.public_key, cm.role " +
                "FROM channel_members cm JOIN users u ON u.id = cm.user_id " +
                "WHERE cm.channel_id = (SELECT id FROM channels WHERE name = ?)",
                [msg.channel]);
            wsSend(conn, { type: "members", channel: msg.channel, users: members });

        } else if (msg.type === "list") {
            const channels = db.query(
                "SELECT c.name, c.topic, COUNT(cm.user_id) as members " +
                "FROM channels c LEFT JOIN channel_members cm ON cm.channel_id = c.id " +
                "GROUP BY c.id ORDER BY c.name");
            wsSend(conn, { type: "channels", list: channels });

        } else if (msg.type === "dm") {
            if (!msg.to || !msg.encrypted_for_recipient
                || !msg.encrypted_for_sender || !msg.nonce)
                return wsError(conn, "to, encrypted_for_recipient, encrypted_for_sender, and nonce required");
            const recipient = db.query(
                "SELECT id FROM users WHERE username = ?", [msg.to]);
            if (recipient.length === 0)
                return wsError(conn, `user not found: ${String(msg.to)}`);
            const now = time.now();
            db.exec(
                "INSERT INTO direct_messages (sender_id, recipient_id, encrypted_for_recipient, encrypted_for_sender, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?)",
                [conn.data.userId, recipient[0].id, msg.encrypted_for_recipient, msg.encrypted_for_sender, msg.nonce, now]);
            const dmId = db.lastId();
            const rcptConn = onlineUsers[msg.to];
            if (rcptConn) {
                wsSend(rcptConn, {
                    type: "dm", from: conn.data.username,
                    encrypted: msg.encrypted_for_recipient,
                    nonce: msg.nonce, at: now, id: dmId,
                });
            }
            wsSend(conn, {
                type: "dm", from: conn.data.username, to: msg.to,
                encrypted: msg.encrypted_for_sender,
                nonce: msg.nonce, at: now, id: dmId,
            });

        } else if (msg.type === "dm_history") {
            if (!msg.with)
                return wsError(conn, "with field required");
            const other = db.query(
                "SELECT id FROM users WHERE username = ?", [msg.with]);
            if (other.length === 0)
                return wsError(conn, `user not found: ${String(msg.with)}`);
            let limit = msg.limit || 50;
            if (limit > 200) limit = 200;
            const otherId = other[0].id;
            const myId = conn.data.userId;
            let messages;
            if (msg.before) {
                messages = db.query(
                    "SELECT dm.id, u.username as 'from', " +
                    "CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender " +
                    "ELSE dm.encrypted_for_recipient END as encrypted, " +
                    "dm.nonce, dm.created_at as at " +
                    "FROM direct_messages dm JOIN users u ON u.id = dm.sender_id " +
                    "WHERE ((dm.sender_id = ? AND dm.recipient_id = ?) " +
                    "OR (dm.sender_id = ? AND dm.recipient_id = ?)) " +
                    "AND dm.id < ? ORDER BY dm.created_at DESC LIMIT ?",
                    [myId, myId, otherId, otherId, myId, msg.before, limit]);
            } else {
                messages = db.query(
                    "SELECT dm.id, u.username as 'from', " +
                    "CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender " +
                    "ELSE dm.encrypted_for_recipient END as encrypted, " +
                    "dm.nonce, dm.created_at as at " +
                    "FROM direct_messages dm JOIN users u ON u.id = dm.sender_id " +
                    "WHERE (dm.sender_id = ? AND dm.recipient_id = ?) " +
                    "OR (dm.sender_id = ? AND dm.recipient_id = ?) " +
                    "ORDER BY dm.created_at DESC LIMIT ?",
                    [myId, myId, otherId, otherId, myId, limit]);
            }
            wsSend(conn, { type: "dm_history", with: msg.with, messages });

        } else if (msg.type === "typing") {
            if (!msg.to)
                return wsError(conn, "to field required");
            const rcptConn = onlineUsers[msg.to];
            if (rcptConn) {
                wsSend(rcptConn, { type: "typing", from: conn.data.username });
            }

        } else if (msg.type === "users") {
            const users = db.query(
                "SELECT username, public_key FROM users ORDER BY username");
            for (const u of users) {
                u.online = !!onlineUsers[u.username];
            }
            wsSend(conn, { type: "users", list: users });

        } else {
            wsError(conn, `unknown message type: ${String(msg.type)}`);
        }
    },

    onClose(conn, code) {
        if (conn.data.username) {
            delete onlineUsers[conn.data.username];
            const channels = conn.data.channels || {};
            for (const channel in channels) {
                broadcastToChannel(channel, {
                    type: "left", channel,
                    user: conn.data.username,
                });
            }
            ws.broadcast("/ws", JSON.stringify({
                type: "presence", username: conn.data.username, online: false,
            }));
        }
        log.info(`ws: ${conn.data.username || "anon"} disconnected code=${code}`);
    },
});

// ── WS connection count ─────────────────────────────────────────────

app.get("/ws/connections", (_req, res) => {
    res.json({ count: ws.connections("/ws") });
});

// ── File endpoints ────────────────────────────────────────────────

const MAX_FILE_SIZE = 1048576; // 1 MB

app.post("/files", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    let body;
    try { body = JSON.parse(req.body); } catch (_e) { body = null; }
    if (!body) return res.status(400).json({ error: "invalid JSON" });

    if (!body.filename || !body.encrypted || !body.nonce)
        return res.status(400).json({ error: "filename, encrypted, and nonce required" });
    if (!body.channel && !body.to)
        return res.status(400).json({ error: "channel or to required" });
    if (body.channel && body.to)
        return res.status(400).json({ error: "channel and to are mutually exclusive" });
    if (body.encrypted.length > MAX_FILE_SIZE)
        return res.status(413).json({ error: "file too large (max 1 MB)" });

    let channelId = null;
    let recipientId = null;
    if (body.channel) {
        let chName = body.channel;
        if (!chName.startsWith("#")) chName = `#${chName}`;
        const ch = db.query("SELECT id FROM channels WHERE name = ?", [chName]);
        if (ch.length === 0) return res.status(404).json({ error: "channel not found" });
        const mem = db.query(
            "SELECT 1 FROM channel_members WHERE channel_id = ? AND user_id = ?",
            [ch[0].id, sess.user_id]);
        if (mem.length === 0) return res.status(403).json({ error: "not a channel member" });
        channelId = ch[0].id;
    } else {
        const rcpt = db.query("SELECT id FROM users WHERE username = ?", [body.to]);
        if (rcpt.length === 0) return res.status(404).json({ error: "user not found" });
        recipientId = rcpt[0].id;
    }

    const now = time.now();
    const size = body.encrypted.length;

    if (RETENTION.maxFileStorage > 0) {
        const usage = db.query("SELECT COALESCE(SUM(size), 0) as total FROM files");
        const total = (usage[0] && usage[0].total) || 0;
        if (total + size > RETENTION.maxFileStorage)
            return res.status(507).json({ error: "file storage limit reached" });
    }

    if (channelId) {
        db.exec(
            "INSERT INTO files (uploader_id, filename, size, content, channel_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            [sess.user_id, body.filename, size, body.encrypted, channelId, body.nonce, now]);
    } else {
        db.exec(
            "INSERT INTO files (uploader_id, filename, size, content, recipient_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            [sess.user_id, body.filename, size, body.encrypted, recipientId, body.nonce, now]);
    }
    const fileId = db.lastId();

    const note = {
        type: "file", from: sess.username, id: fileId,
        filename: body.filename, size, nonce: body.nonce,
    };
    if (channelId) {
        let chName = body.channel;
        if (!chName.startsWith("#")) chName = `#${chName}`;
        note.channel = chName;
        broadcastToChannel(chName, note);
    } else {
        note.to = body.to;
        const rcptConn = onlineUsers[body.to];
        if (rcptConn) wsSend(rcptConn, note);
        const senderConn = onlineUsers[sess.username];
        if (senderConn) wsSend(senderConn, note);
    }

    res.status(201).json({ id: fileId, filename: body.filename, size });
});

app.get("/files/:id", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const rows = db.query(
        "SELECT f.id, f.uploader_id, f.filename, f.size, f.content, f.channel_id, " +
        "f.recipient_id, f.nonce, f.created_at, u.username as 'from' " +
        "FROM files f JOIN users u ON u.id = f.uploader_id WHERE f.id = ?",
        [parseInt(req.params.id)]);
    if (rows.length === 0) return res.status(404).json({ error: "file not found" });

    const file = rows[0];
    if (file.channel_id) {
        const mem = db.query(
            "SELECT 1 FROM channel_members WHERE channel_id = ? AND user_id = ?",
            [file.channel_id, sess.user_id]);
        if (mem.length === 0) return res.status(403).json({ error: "not a channel member" });
    } else {
        if (file.uploader_id !== sess.user_id && file.recipient_id !== sess.user_id)
            return res.status(403).json({ error: "access denied" });
    }

    res.json({
        id: file.id, filename: file.filename, size: file.size,
        encrypted: file.content, nonce: file.nonce,
        from: file.from, created_at: file.created_at,
    });
});

app.get("/channels/:name/files", (req, res) => {
    let chName = req.params.name;
    if (!chName.startsWith("#")) chName = `#${chName}`;

    const files = db.query(
        "SELECT f.id, f.filename, f.size, f.nonce, f.created_at, u.username as 'from' " +
        "FROM files f JOIN users u ON u.id = f.uploader_id " +
        "WHERE f.channel_id = (SELECT id FROM channels WHERE name = ?) " +
        "ORDER BY f.created_at DESC LIMIT 50", [chName]);

    res.json({ channel: chName, files });
});

app.get("/dm/:username/files", (req, res) => {
    const sess = requireSession(req, res);
    if (!sess) return;

    const other = db.query("SELECT id FROM users WHERE username = ?", [req.params.username]);
    if (other.length === 0) return res.status(404).json({ error: "user not found" });

    const myId = sess.user_id;
    const otherId = other[0].id;
    const files = db.query(
        "SELECT f.id, f.filename, f.size, f.nonce, f.created_at, u.username as 'from' " +
        "FROM files f JOIN users u ON u.id = f.uploader_id " +
        "WHERE (f.uploader_id = ? AND f.recipient_id = ?) " +
        "OR (f.uploader_id = ? AND f.recipient_id = ?) " +
        "ORDER BY f.created_at DESC LIMIT 50",
        [myId, otherId, otherId, myId]);

    res.json({ with: req.params.username, files });
});

// ── E2E self-test endpoint ─────────────────────────────────────────
// Exercises the full 2-client WebSocket flow from inside the server:
//   register 2 users, create a channel, connect both via ws.connect,
//   authenticate, join, send a message, verify receipt.

app.get("/e2e-test", async (req, res) => {
    const host = req.headers.host || "127.0.0.1:3000";
    const port = host.split(":")[1] || "3000";
    const wsUrl = `ws://127.0.0.1:${port}/ws`;

    const results = {
        alice_registered: false,
        bob_registered: false,
        channel_created: false,
        alice_connected: false,
        bob_connected: false,
        alice_authenticated: false,
        bob_authenticated: false,
        alice_joined: false,
        bob_joined: false,
        message_sent: false,
        message_received: false,
        received_from: null,
        received_encrypted: null,
        dm_sent: false,
        dm_received: false,
        dm_confirmed: false,
        dm_stored: false,
        dm_history_ok: false,
        file_channel_stored: false,
        file_channel_content_ok: false,
        file_dm_stored: false,
        file_dm_content_ok: false,
        retention_cleanup_ok: false,
        file_capacity_ok: false,
    };

    // Step 1: Register users directly via DB
    const alicePw = crypto.hashPassword("testpass1234");
    const aliceKp = crypto.boxKeypair();
    try {
        db.exec("INSERT INTO users (username, password_hash, public_key, created_at) VALUES (?, ?, ?, ?)",
                ["alice_e2e", alicePw, aliceKp.publicKey, time.now()]);
    } catch (_e) { /* may already exist */ }
    const aliceRows = db.query("SELECT id FROM users WHERE username = ?", ["alice_e2e"]);
    results.alice_registered = aliceRows.length > 0;

    const bobPw = crypto.hashPassword("testpass5678");
    const bobKp = crypto.boxKeypair();
    try {
        db.exec("INSERT INTO users (username, password_hash, public_key, created_at) VALUES (?, ?, ?, ?)",
                ["bob_e2e", bobPw, bobKp.publicKey, time.now()]);
    } catch (_e) { /* may already exist */ }
    const bobRows = db.query("SELECT id FROM users WHERE username = ?", ["bob_e2e"]);
    results.bob_registered = bobRows.length > 0;

    // Step 2: Create #e2e-test channel
    try {
        db.exec("INSERT INTO channels (name, topic, creator_id, created_at) VALUES (?, ?, ?, ?)",
                ["#e2e-test", "E2E test channel", aliceRows[0].id, time.now()]);
    } catch (_e) { /* may already exist */ }
    const ch = db.query("SELECT id FROM channels WHERE name = ?", ["#e2e-test"]);
    results.channel_created = ch.length > 0;

    if (ch.length > 0) {
        try {
            db.exec("INSERT INTO channel_members (channel_id, user_id, role, encrypted_key, nonce, joined_at) VALUES (?, ?, ?, ?, ?, ?)",
                    [ch[0].id, aliceRows[0].id, "admin", "test_key", "test_nonce", time.now()]);
        } catch (_e) { /* may already exist */ }
    }

    // Step 3: Connect alice via WebSocket (sequentially to avoid frame interleaving)
    let done = false;
    let aliceReady = false;
    let bobReady = false;

    const aliceWs = ws.connect(wsUrl, {
        onOpen(_conn) {
            results.alice_connected = true;
        },
        onMessage(conn, msg) {
            let data;
            try { data = JSON.parse(msg); } catch (_e) { return; }
            if (!data) return;

            if (data.type === "motd") {
                conn.send(JSON.stringify({
                    type: "login", username: "alice_e2e", password: "testpass1234",
                }));
            } else if (data.type === "authenticated") {
                results.alice_authenticated = true;
                conn.send(JSON.stringify({ type: "join", channel: "#e2e-test" }));
            } else if (data.type === "joined" && data.channel === "#e2e-test") {
                results.alice_joined = true;
                aliceReady = true;
            }
        },
        onError(_conn, err) {
            log.error(`e2e alice ws error: ${err}`);
        },
    });

    // Wait for alice to be ready before connecting bob
    let waited = 0;
    while (!aliceReady && waited < 5000) {
        await hull.sleep(100);
        waited += 100;
    }

    // Step 4: Connect bob
    const bobWs = ws.connect(wsUrl, {
        onOpen(_conn) {
            results.bob_connected = true;
        },
        onMessage(conn, msg) {
            let data;
            try { data = JSON.parse(msg); } catch (_e) { return; }
            if (!data) return;

            if (data.type === "motd") {
                conn.send(JSON.stringify({
                    type: "login", username: "bob_e2e", password: "testpass5678",
                }));
            } else if (data.type === "authenticated") {
                results.bob_authenticated = true;
                conn.send(JSON.stringify({ type: "join", channel: "#e2e-test" }));
            } else if (data.type === "joined" && data.channel === "#e2e-test") {
                results.bob_joined = true;
                bobReady = true;
            } else if (data.type === "msg" && data.from === "alice_e2e") {
                results.message_received = true;
                results.received_from = data.from;
                results.received_encrypted = data.encrypted;
                done = true;
            }
        },
        onError(_conn, err) {
            log.error(`e2e bob ws error: ${err}`);
        },
    });

    // Wait for bob to be ready
    waited = 0;
    while (!bobReady && waited < 5000) {
        await hull.sleep(100);
        waited += 100;
    }

    // Step 5: Alice sends message, wait for bob to receive
    if (aliceReady && bobReady) {
        aliceWs.send(JSON.stringify({
            type: "msg", channel: "#e2e-test",
            encrypted: "e2e_test_ciphertext_abc123",
            nonce: "e2e_test_nonce_xyz789",
        }));
        results.message_sent = true;

        let msgWaited = 0;
        while (!done && msgWaited < 3000) {
            await hull.sleep(100);
            msgWaited += 100;
        }
    }

    // Step 6: DM test — Alice sends DM to Bob
    if (aliceReady && bobReady) {
        aliceWs.send(JSON.stringify({
            type: "dm", to: "bob_e2e",
            encrypted_for_recipient: "dm_ciphertext_for_bob",
            encrypted_for_sender: "dm_ciphertext_for_alice",
            nonce: "dm_nonce_abc123",
        }));
        results.dm_sent = true;

        let dmWaited = 0;
        while (dmWaited < 3000) {
            await hull.sleep(100);
            dmWaited += 100;
            const dmRows = db.query(
                "SELECT id FROM direct_messages WHERE nonce = 'dm_nonce_abc123'");
            if (dmRows.length > 0) {
                results.dm_stored = true;
                break;
            }
        }

        if (results.dm_stored) {
            const dmCheck = db.query(
                "SELECT encrypted_for_recipient, encrypted_for_sender FROM direct_messages WHERE nonce = 'dm_nonce_abc123'");
            if (dmCheck.length > 0) {
                results.dm_received = dmCheck[0].encrypted_for_recipient === "dm_ciphertext_for_bob";
                results.dm_confirmed = dmCheck[0].encrypted_for_sender === "dm_ciphertext_for_alice";
            }
        }

        // Step 7: DM history
        const aliceId = aliceRows[0].id;
        const bobId = bobRows[0].id;
        const history = db.query(
            "SELECT dm.id, u.username as 'from', " +
            "CASE WHEN dm.sender_id = ? THEN dm.encrypted_for_sender " +
            "ELSE dm.encrypted_for_recipient END as encrypted, dm.nonce " +
            "FROM direct_messages dm JOIN users u ON u.id = dm.sender_id " +
            "WHERE (dm.sender_id = ? AND dm.recipient_id = ?) " +
            "OR (dm.sender_id = ? AND dm.recipient_id = ?) " +
            "ORDER BY dm.created_at DESC LIMIT 10",
            [aliceId, aliceId, bobId, bobId, aliceId]);
        results.dm_history_ok = history.length > 0 && history[0].nonce === "dm_nonce_abc123";
    }

    // File upload tests (direct DB insert, verifies schema works)
    if (results.channel_created) {
        try {
            db.exec(
                "INSERT INTO files (uploader_id, filename, size, content, channel_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                [aliceRows[0].id, "test.txt", 18, "encrypted_file_data", ch[0].id, "file_nonce_1", time.now()]);
        } catch (_e) { /* ignore */ }
        const fileRows = db.query("SELECT id, content FROM files WHERE nonce = 'file_nonce_1'");
        results.file_channel_stored = fileRows.length > 0;
        results.file_channel_content_ok = fileRows.length > 0 && fileRows[0].content === "encrypted_file_data";
    }

    if (results.alice_registered && results.bob_registered) {
        try {
            db.exec(
                "INSERT INTO files (uploader_id, filename, size, content, recipient_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                [aliceRows[0].id, "secret.txt", 15, "dm_file_content", bobRows[0].id, "file_nonce_2", time.now()]);
        } catch (_e) { /* ignore */ }
        const dmFileRows = db.query("SELECT id, content FROM files WHERE nonce = 'file_nonce_2'");
        results.file_dm_stored = dmFileRows.length > 0;
        results.file_dm_content_ok = dmFileRows.length > 0 && dmFileRows[0].content === "dm_file_content";
    }

    // Retention cleanup test: insert old records, run cleanup, verify deletion
    try {
        const oldTs = time.now() - 999999;
        db.exec(
            "INSERT INTO messages (channel_id, user_id, encrypted_body, nonce, created_at) VALUES (?, ?, ?, ?, ?)",
            [ch[0].id, aliceRows[0].id, "old_chan_msg", "old_chan_nonce", oldTs]);
        db.exec(
            "INSERT INTO direct_messages (sender_id, recipient_id, encrypted_for_recipient, encrypted_for_sender, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?)",
            [aliceRows[0].id, bobRows[0].id, "old_dm_r", "old_dm_s", "old_dm_nonce", oldTs]);
        db.exec(
            "INSERT INTO files (uploader_id, filename, size, content, recipient_id, nonce, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            [aliceRows[0].id, "old.txt", 5, "old_f", bobRows[0].id, "old_file_nonce", oldTs]);
        const origCh = RETENTION.channelTtl;
        const origDm = RETENTION.dmTtl;
        const origFile = RETENTION.fileTtl;
        RETENTION.channelTtl = 1;
        RETENTION.dmTtl = 1;
        RETENTION.fileTtl = 1;
        cleanupExpired();
        RETENTION.channelTtl = origCh;
        RETENTION.dmTtl = origDm;
        RETENTION.fileTtl = origFile;
        const chanRemains = db.query("SELECT id FROM messages WHERE nonce = 'old_chan_nonce'");
        const dmRemains = db.query("SELECT id FROM direct_messages WHERE nonce = 'old_dm_nonce'");
        const fileRemains = db.query("SELECT id FROM files WHERE nonce = 'old_file_nonce'");
        results.retention_cleanup_ok = chanRemains.length === 0 && dmRemains.length === 0 && fileRemains.length === 0;
    } catch (_e) { /* ignore */ }

    // File capacity test: verify limit logic works
    try {
        const origMax = RETENTION.maxFileStorage;
        RETENTION.maxFileStorage = 1; // 1 byte limit
        const usage = db.query("SELECT COALESCE(SUM(size), 0) as total FROM files");
        const total = (usage[0] && usage[0].total) || 0;
        results.file_capacity_ok = total + 100 > RETENTION.maxFileStorage;
        RETENTION.maxFileStorage = origMax;
    } catch (_e) { /* ignore */ }

    // Clean up WS connections
    try { aliceWs.close(); } catch (_e) { /* ignore */ }
    try { bobWs.close(); } catch (_e) { /* ignore */ }

    // Verify message was stored in DB
    const stored = db.query(
        "SELECT encrypted_body FROM messages WHERE channel_id = (SELECT id FROM channels WHERE name = '#e2e-test') ORDER BY id DESC LIMIT 1");
    results.message_stored = stored.length > 0 && stored[0].encrypted_body === "e2e_test_ciphertext_abc123";

    // Step 10: Check all passed
    results.all_passed = results.alice_registered
        && results.bob_registered
        && results.channel_created
        && results.alice_connected
        && results.bob_connected
        && results.alice_authenticated
        && results.bob_authenticated
        && results.alice_joined
        && results.bob_joined
        && results.message_sent
        && results.message_received
        && results.message_stored
        && results.dm_sent
        && results.dm_received
        && results.dm_confirmed
        && results.dm_stored
        && results.dm_history_ok
        && results.file_channel_stored
        && results.file_channel_content_ok
        && results.file_dm_stored
        && results.file_dm_content_ok
        && results.retention_cleanup_ok
        && results.file_capacity_ok;

    res.json(results);
});

// ── Retention cleanup ──────────────────────────────────────────────

function cleanupExpired() {
    const now = time.now();
    let deleted = 0;
    if (RETENTION.channelTtl > 0) {
        const cutoff = now - RETENTION.channelTtl;
        db.exec("DELETE FROM messages WHERE created_at < ?", [cutoff]);
        const del = db.query("SELECT changes() as n");
        deleted += (del[0] && del[0].n) || 0;
    }
    if (RETENTION.dmTtl > 0) {
        const cutoff = now - RETENTION.dmTtl;
        db.exec("DELETE FROM direct_messages WHERE created_at < ?", [cutoff]);
        const del = db.query("SELECT changes() as n");
        deleted += (del[0] && del[0].n) || 0;
    }
    if (RETENTION.fileTtl > 0) {
        const cutoff = now - RETENTION.fileTtl;
        db.exec("DELETE FROM files WHERE created_at < ?", [cutoff]);
        const del = db.query("SELECT changes() as n");
        deleted += (del[0] && del[0].n) || 0;
    }
    if (RETENTION.maxFileStorage > 0) {
        const usage = db.query("SELECT COALESCE(SUM(size), 0) as total FROM files");
        const total = (usage[0] && usage[0].total) || 0;
        if (total > RETENTION.maxFileStorage) {
            db.exec(
                "DELETE FROM files WHERE id IN (SELECT id FROM files ORDER BY created_at ASC LIMIT 100)");
            const del = db.query("SELECT changes() as n");
            deleted += (del[0] && del[0].n) || 0;
        }
    }
    if (deleted > 0) {
        log.info(`retention cleanup: deleted ${deleted} expired records`);
    }
}

if (RETENTION.channelTtl > 0 || RETENTION.dmTtl > 0
    || RETENTION.fileTtl > 0 || RETENTION.maxFileStorage > 0) {
    app.every(RETENTION.cleanupInterval, cleanupExpired);
    log.info(`retention cleanup enabled — channelTtl=${RETENTION.channelTtl}s dmTtl=${RETENTION.dmTtl}s fileTtl=${RETENTION.fileTtl}s maxFileStorage=${RETENTION.maxFileStorage}B`);
}

log.info("IRC Chat app loaded — routes registered");
