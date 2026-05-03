# IRC Chat Example — Roadmap

## Current (v1)

- [x] Registration + login (PBKDF2 password hashing)
- [x] Session-based authentication
- [x] WebSocket real-time messaging
- [x] Channels (create, join, leave, list, topic)
- [x] Channel member management (who, kick)
- [x] E2E encryption (Curve25519 key exchange + XSalsa20-Poly1305 secretbox)
- [x] Encrypted message history (DB stores ciphertext only)
- [x] MOTD on WebSocket connect
- [x] Both Lua and JS implementations

## Planned: Federation (v2)

Multi-server IRC chat where channels span across Hull instances.

**Protocol:**
- Servers connect to each other via `ws.connect()` (client WebSocket)
- Server-to-server messages are authenticated via Ed25519 signatures
- Channel membership replicated across federated servers
- Messages relayed between servers (still E2E encrypted)

**API:**
```lua
-- In app config
federation.add_peer("wss://other-server.example.com/federation", {
    key = "peer_public_key_hex",
})

-- Federated channels prefixed with server name
-- #general           → local channel
-- other.com:#general  → federated channel on other.com
```

**Tasks:**
- [ ] Server-to-server WebSocket protocol
- [ ] Ed25519 mutual authentication between peers
- [ ] Channel federation (replicate membership + messages)
- [ ] Split-brain resolution (vector clocks or CRDT)
- [ ] Federated user directory (user@server.com)

## Planned: File Transfer (v3)

Encrypted file sharing within channels or DMs.

**Protocol:**
- Files encrypted with channel key (same as messages)
- Uploaded via HTTP POST, stored in `fs.write()`
- File metadata sent via WebSocket: `{ type: "file", channel, filename, size, encrypted_url }`
- Recipients download via HTTP GET with auth

**API:**
```lua
-- Upload
POST /channels/:name/files
Content-Type: multipart/form-data
→ { id, filename, size, url }

-- Download (auth required, must be channel member)
GET /files/:id

-- WS notification to channel members
{ "type": "file", "channel": "#general", "from": "alice",
  "filename": "doc.pdf", "size": 1234, "url": "/files/42" }
```

**Tasks:**
- [ ] File upload endpoint (multipart form data)
- [ ] File encryption with channel key before storage
- [ ] File metadata in messages table
- [ ] Download endpoint with membership check
- [ ] File size limits (manifest-configurable)
- [ ] Thumbnail generation for images (via `image` module)

## Planned: Direct Messages (v4)

Private 1:1 encrypted messaging.

**Protocol:**
- DM uses `crypto.box(msg, nonce, recipient_pk, sender_sk)` — no shared channel key
- Each DM is encrypted specifically for the recipient
- DM history stored encrypted per-pair

**Tasks:**
- [ ] DM send/receive via WebSocket
- [ ] Per-pair crypto.box encryption
- [ ] DM history endpoint
- [ ] Online status / typing indicators
