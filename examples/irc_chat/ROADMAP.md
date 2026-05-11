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

## Current (v3)

- [x] File upload endpoint (POST /files with JSON body)
- [x] File download endpoint with access control (GET /files/:id)
- [x] Channel file listing (GET /channels/:name/files)
- [x] DM file listing (GET /dm/:username/files)
- [x] Encrypted file content stored in DB (server never sees plaintext)
- [x] Channel files: membership check on upload + download
- [x] DM files: sender/recipient access check
- [x] WS notification on file upload (channel broadcast or DM delivery)
- [x] 1 MB file size limit
- [x] E2E test coverage (HTTP + DB verification)

## Current (v4)

- [x] DM send/receive via WebSocket (crypto.box per-pair encryption)
- [x] Two encrypted copies per DM (one for recipient, one for sender history)
- [x] DM history endpoint (HTTP + WebSocket, cursor pagination)
- [x] Online user tracking + presence broadcasts
- [x] Typing indicators (ephemeral relay)
- [x] User directory endpoint (GET /users, WS `users` command)
- [x] E2E test coverage for DM flow
