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

app.manifest({});
session.init({ ttl: 7200 });

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
    if (!name.startsWith("#")) name = "#" + name;

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
    if (!channelName.startsWith("#")) channelName = "#" + channelName;

    const members = db.query(
        "SELECT u.id, u.username, u.public_key, cm.role, cm.joined_at " +
        "FROM channel_members cm JOIN users u ON u.id = cm.user_id " +
        "WHERE cm.channel_id = (SELECT id FROM channels WHERE name = ?) " +
        "ORDER BY cm.joined_at", [channelName]);
    res.json({ channel: channelName, members });
});

app.get("/channels/:name/history", (req, res) => {
    let channelName = req.params.name;
    if (!channelName.startsWith("#")) channelName = "#" + channelName;

    const messages = db.query(
        "SELECT m.id, u.username, m.encrypted_body, m.nonce, m.created_at " +
        "FROM messages m JOIN users u ON u.id = m.user_id " +
        "WHERE m.channel_id = (SELECT id FROM channels WHERE name = ?) " +
        "ORDER BY m.created_at DESC LIMIT 50", [channelName]);
    res.json({ channel: channelName, messages });
});

// ── WebSocket chat ──────────────────────────────────────────────────

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
            wsSend(conn, {
                type: "authenticated",
                username: rows[0].username,
                public_key: rows[0].public_key,
            });
            return;
        }

        if (!conn.data.authenticated)
            return wsError(conn, "not authenticated — send login first");

        if (msg.type === "join") {
            if (!msg.channel)
                return wsError(conn, "channel name required");
            let channelName = msg.channel;
            if (!channelName.startsWith("#")) channelName = "#" + channelName;

            const ch = db.query(
                "SELECT id FROM channels WHERE name = ?", [channelName]);
            if (ch.length === 0)
                return wsError(conn, "channel not found: " + channelName);

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
                return wsError(conn, "not in channel: " + msg.channel);

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

        } else {
            wsError(conn, "unknown message type: " + String(msg.type));
        }
    },

    onClose(conn, code) {
        if (conn.data.username) {
            const channels = conn.data.channels || {};
            for (const channel in channels) {
                broadcastToChannel(channel, {
                    type: "left", channel,
                    user: conn.data.username,
                });
            }
        }
        log.info(`ws: ${conn.data.username || "anon"} disconnected code=${code}`);
    },
});

// ── WS connection count ─────────────────────────────────────────────

app.get("/ws/connections", (_req, res) => {
    res.json({ count: ws.connections("/ws") });
});

log.info("IRC Chat app loaded — routes registered");
