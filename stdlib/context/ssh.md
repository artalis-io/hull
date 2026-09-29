<!-- minimal -->
## SSH client (`hull/ssh@1`, Lua only)

Hull speaks SSH-2 itself: no `ssh` binary, no agent, no `~/.ssh`, no shell
quoting. The hosts, ports and logins an app may reach are declared in the
manifest and enforced in C before a socket exists.

```lua
app.manifest({
    modules = { "hull/ssh@1", "hull/fs@1" },
    fs  = { read = { "id_ed25519" } },
    ssh = { connect = { hosts = { "build.internal" }, ports = { 22 },
                        users = { "deploy" } } },
})

local ssh = require("hull.ssh")
local fs  = require("hull.fs")

app.main(function(ctx)
    local trust = ssh.memory_store()
    local conn, err = ssh.connect{ host = "build.internal", user = "deploy",
                                   key = fs.read("id_ed25519"), trust = trust }
    if not conn then ctx.stderr:write(err.code .. "\n"); return 1 end
    local r = conn:exec("uname -a")          -- { status, stdout, stderr, signal }
    ctx.stdout:write(r.stdout)
    conn:close()
    return r.status
end)
```

Run with `hull run app.lua`. Every call returns a value or `nil, { code, detail }`
- never raises (only your own callback's error is re-raised). A non-zero
remote exit is `r.status`, not an error.

<!-- compact -->
## The manifest is the allowlist

```lua
ssh = {
    connect = { hosts = { "*.internal", "10.0.0.0/8", "$FLEET_HOST" },
                ports = { 22 }, users = { "deploy" } },   -- all three required
    tunnel  = { hosts = { "relay.example.com" }, ports = { 443 } },  -- only for relays
}
```

A host, port or login outside the lists is `{ code = "denied" }`. `tunnel` is
a second grant: going through a relay needs the destination in `connect` AND
the relay in `tunnel`.

## Host keys: Hull refuses, you decide

An unknown host is refused with `host_unknown` and a `fingerprint` (the one
`ssh-keygen -l` prints) plus `key_blob`. Accepting is an explicit act:

```lua
local conn, err = ssh.connect(opts)
if not conn and err.code == "host_unknown" then
    -- show err.fingerprint to an operator, or check it against a known value
    ssh.accept_host(trust, host, err.key_blob, port)
    conn, err = ssh.connect(opts)
end
```

`host_changed` is what a man-in-the-middle looks like: never auto-accept it.
Trust stores: `ssh.memory_store()`, `ssh.file_store(path)` (OpenSSH
`known_hosts` format, hashed entries supported), `ssh.kv_store(kv)` (shared
across workers via `hull/kv`). Host keys verified: Ed25519, ECDSA P-256/P-384,
RSA (SHA-2, >= 2048 bits).

## Keys

`key` is the TEXT of an OpenSSH private key file: Ed25519, RSA (>= 2048
bits) or ECDSA P-256/P-384. A passphrase-protected key needs
`passphrase_env = "VAR"` (preferred: read and scrubbed in C, never a Lua
string) or `passphrase = "..."`.

## Commands

```lua
conn:exec(cmd, {
    stdin      = "data",                    -- up to 128 KiB, then EOF
    on_stdout  = function(chunk) ... end,   -- stream instead of buffering
    on_stderr  = function(chunk) ... end,
    max_output = 8 * 1024 * 1024,           -- buffered cap -> output_too_large
    timeout_ms = 30000,                     -- whole command -> timeout
})
```

## SFTP

```lua
local f = assert(conn:sftp())
f:write("remote.txt", data)
local back = f:read("remote.txt")           -- bounded (16 MiB default)
local entries = f:list(".")                 -- names safe to use locally
f:stat("remote.txt"); f:remove("remote.txt"); f:mkdir("dir")
f:close()
```

Paths are length-prefixed strings inside the protocol: a filename never
becomes a shell word. `f:open(path, mode)` streams files larger than memory.

<!-- full -->
## Through a WebSocket relay (Cloudflare Access style)

```lua
ssh.connect{ host = "10.0.0.7", user = "deploy", key = key, trust = trust,
             tunnel = { host = "relay.example.com", port = 443, tls = true,
                        headers = { "Cf-Access-Client-Id: ...",
                                    "Cf-Access-Client-Secret: ..." } } }
```

## Timeouts, keepalives, rekeying

- `timeout_ms` on `connect` bounds reaching an authenticated connection.
- Keepalives run once authenticated (`keepalive_ms`, `keepalive_max`); a
  silent server ends as `timeout` rather than a hang.
- Keys are renegotiated after 1 GiB per direction; the server's own rekeys are
  absorbed mid-stream. `conn:rekey()` forces one; `conn:stats()` reports
  bytes, packets and rekeys.
- An SFTP session waits at most `reply_timeout_ms` (default 60000) for the
  server's next message: `conn:sftp({ reply_timeout_ms = 30000 })`.

## What is negotiated

curve25519-sha256 key exchange, AES-256-GCM, strict KEX (Terrapin). Refused
by construction: SSH-1, SHA-1 `ssh-rsa`, DSA, CBC, MD5, weak DH, compression.
P-521 keys are not supported.

## Error codes to branch on

`denied`, `connect_failed`, `timeout`, `host_unknown`, `host_changed`,
`host_revoked`, `host_key_invalid`, `auth_failed`, `partial_success`,
`bad_key`, `bad_passphrase`, `passphrase_required`, `unsupported_key_type`,
`output_too_large`, `stdin_too_large`, SFTP statuses (`no_such_file`,
`permission_denied`, ...). The full table: `docs/ssh.md` section 10.

Worked example: `examples/ssh_fleet/`.
