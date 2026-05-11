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

## Current (v2) — Federation

Relay-only federation: servers connect via `ws.connect()`, authenticate with Ed25519 challenge-response, and relay channel messages + presence between peers. No shared membership DB, no CRDTs — each server owns its users and channels.

- [x] Server-to-server WebSocket protocol (`/federation` endpoint)
- [x] Ed25519 mutual authentication (challenge-response handshake)
- [x] Channel message relay (`fed_msg` for configured channels)
- [x] Join/leave relay (`fed_join`, `fed_leave`)
- [x] Presence relay (`fed_presence`)
- [x] Loop prevention (federated messages never re-relayed)
- [x] Outbound peer connections with auto-reconnect
- [x] Federation status HTTP endpoint (`GET /federation/status`)
- [x] Loopback E2E test (`GET /e2e-federation-test`)
- [x] Both Lua and JS implementations

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
