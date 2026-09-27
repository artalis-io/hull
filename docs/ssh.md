# `hull/ssh` - the SSH-2 client

Hull speaks SSH-2 itself. There is no `ssh` binary to find, no agent to talk
to, no `~/.ssh` to read, and no shell to quote through: a Hull app that needs
to reach a machine opens the connection from inside the runtime, under the
same manifest that gates every other capability.

That matters most for the thing an operator actually builds with it - a fleet
tool, a deploy step, a health sweep - because the set of hosts it may reach is
declared up front and enforced in C, not assembled at runtime from strings.

Worked example: [`examples/ssh_fleet/`](../examples/ssh_fleet/app.lua).
Design record: [`ssh_module_design.md`](ssh_module_design.md).

**Lua only.** There is no JS implementation today; see
[`ssh_module_design.md` §8](ssh_module_design.md) for why, and what it would
take.

## 1. The shortest thing that works

```lua
app.manifest({
    modules = { "hull/ssh@1", "hull/fs@1" },
    fs = { read = { "id_ed25519" } },
    ssh = {
        connect = { hosts = { "build.internal" }, ports = { 22 },
                    users = { "deploy" } },
    },
})

local ssh = require("hull.ssh")
local fs  = require("hull.fs")

app.main(function(ctx)
    local trust = ssh.memory_store()          -- see §3: this is a decision
    local conn, err = ssh.connect{
        host = "build.internal", user = "deploy",
        key = fs.read("id_ed25519"), trust = trust,
    }
    if not conn then
        ctx.stderr:write(err.code .. "\n")
        return 1
    end
    local r = conn:exec("uname -a")
    ctx.stdout:write(r.stdout)
    conn:close()
    return r.status
end)
```

Run it the way you run any `app.main` program: `hull run app.lua`.

## 2. The manifest is the allowlist

```lua
ssh = {
    connect = {
        hosts = { "*.internal", "10.0.0.7" },   -- exact, "*.suffix", CIDR, "*"
        ports = { 22, 2222 },                   -- integers, not env refs
        users = { "deploy", "root" },
    },
    tunnel = {                                  -- only when using §5
        hosts = { "relay.example.com" },
        ports = { 443 },
    },
}
```

Three properties worth being precise about:

- **It is checked before a socket exists.** `ssh.connect` asks the capability
  layer first; a host outside `connect.hosts` never reaches a DNS lookup, let
  alone a TCP connect. The refusal is `{ code = "denied" }` and the detail
  names which list refused it.
- **The login is part of the grant.** `users` is not decoration: a program
  granted `deploy` cannot connect as `root` by changing one argument.
- **`tunnel` is a second grant, not a widening of the first.** Reaching a host
  through a relay requires BOTH the destination to be in `connect` and the
  relay to be in `tunnel`. An allowed relay is not a way to reach everything
  behind it; that case has its own test in `tests/e2e_ssh_tunnel.sh`.

Only `hosts` entries may be `"$VAR"` env references. Ports are integers,
because a port read from the environment is a grant that changes shape after
the manifest was reviewed.

## 3. Host keys: Hull refuses, you decide

Hull ships no on-disk `known_hosts`, and `ssh.connect` never trusts a host key
on its own. An unknown host comes back as a structured refusal:

```lua
local conn, err = ssh.connect{ ... }
if not conn and err.code == "host_unknown" then
    -- err.fingerprint  "SHA256:..."   show this to a person
    -- err.key_blob     raw bytes      what you would be trusting
end
```

| `err.code` | what happened | the usual response |
|---|---|---|
| `host_unknown` | no stored key for this host | show the fingerprint, accept only on an explicit instruction |
| `host_changed` | the stored key is not the key presented | **stop**; `err.stored_fingerprint` and `err.fingerprint` say what changed |
| `host_key_invalid` | the signature over the exchange hash did not verify | stop; this is not a trust question |
| `host_changed_midsession` | a rekey re-presented a DIFFERENT host key | stop; a server cannot swap identity halfway through |

To trust one, and only then:

```lua
ssh.accept_host(trust, host, err.key_blob)         -- port 22
ssh.accept_host(trust, host, err.key_blob, 2222)   -- any other port
```

A key is trusted for the host **and port** it was met on, and host names are
compared case-insensitively - OpenSSH's `known_hosts` convention. Entries are
named `host` for port 22 and `[host]:port` otherwise, so two sshds on one
machine keep separate keys. `ssh.forget_host(trust, host, port)` takes the
same arguments.

The store is an object with `get`/`put`/`forget`/`entries`.
`ssh.memory_store(seed)` is the in-process one; persisting it is the
application's job, because where trust lives (a file, a DB row, a config map,
nowhere) is a deployment decision rather than a library default. The example
writes it to JSON beside the app.

`host_changed_midsession` deserves its own note: a rekey re-presents the host
key, and Hull pins it against the key **this connection was built on**, not
merely against the store. A server therefore cannot present one identity at
handshake and another at rekey, even one the store would have accepted.

## 4. Keys and passphrases

`ssh.load_key(text, opts)` reads an OpenSSH private key
(`-----BEGIN OPENSSH PRIVATE KEY-----`, ed25519). PEM / PKCS#8 keys are
refused by name rather than by a generic parse error.

An encrypted key needs its passphrase **named, not carried**:

```lua
local key = ssh.load_key(fs.read("id_ed25519"), {
    passphrase_env = "DEPLOY_KEY_PASSPHRASE",   -- the NAME of the variable
})
```

`passphrase_env` hands the variable name to C, which reads the value, runs
`bcrypt_pbkdf`, derives the key and scrubs every buffer it touched. The
passphrase never becomes a Lua string - a Lua string cannot be wiped, and
would sit in the VM's heap until a collection that may never come. The
variable must also be in `manifest.env`.

`opts.passphrase` takes the bytes directly. It exists for the caller who
already holds them, and carries exactly the weakness above.

**Be precise about what that buys.** The guarantee is *the passphrase never
becomes a Lua value* - not *key material is scrubbed end to end*. What comes
back from the derivation is a Lua string, and so is the decrypted private key,
and so is the hex copy made each time it signs. None of them can be wiped, and
the private key has to stay reachable for as long as the connection uses it.
So `passphrase_env` protects the one secret that does **not** have to live in
the VM; everything downstream of it does, and an attacker who can read the
process's Lua heap gets the key either way.

The round count in the key file is also bounded (`crypto.BCRYPT_MAX_ROUNDS`,
2^20). It is read from the file rather than chosen by Hull, the derivation
cannot be interrupted, and each round costs milliseconds - so a key declaring
a few million rounds is not a slow key, it is a stalled process.

## 5. Through a WebSocket relay

A Cloudflare Access tunnel (and most things shaped like one) is TLS carrying a
WebSocket carrying raw TCP. Hull speaks that directly - no `cloudflared`, no
subprocess:

```lua
local conn, err = ssh.connect{
    host = "web1.internal", user = "deploy", key = key, trust = trust,
    tunnel = {
        host = "relay.example.com",
        port = 443,
        tls  = true,
        headers = {
            "Cf-Access-Client-Id: " .. id,
            "Cf-Access-Client-Secret: " .. secret,
        },
    },
}
```

The header that tells the relay which machine to reach is written by Hull,
from `host` and `port`: `Cf-Access-Jump-Destination: web1.internal:22`. The
manifest grant is checked against `host`, and a relay connects wherever that
header says, so letting the app write it freely would let an app granted
`web1` reach any machine behind the relay. A caller-supplied one is accepted
only if it names exactly the granted destination; anything else is `denied`
before a socket is opened. A relay that routes by a different header names it
with `tunnel.destination_header = "X-Target"`. `destination_header = false`
sends none, for a relay that routes some other way (a path, a fixed
backend) - and then the grant does not constrain what the relay reaches, so
that is a statement about the relay, not a default.

`tls = true` verifies the relay's certificate against Hull's trust anchor (the
embedded Mozilla bundle, or `--ca-bundle PATH` for an internal CA; see
[`../CLAUDE.md`](../CLAUDE.md) "HTTPS / CA bundle"). A relay that refuses the
upgrade is reported as `upgrade_refused` with the HTTP status, distinct from
`denied` - an Access policy saying no and the manifest saying no send an
operator to two different files.

## 6. Running commands

```lua
local r = conn:exec("systemctl is-active app")
-- r = { status, signal, stdout, stderr }
```

`r.status` is the remote exit status. A non-zero one is an **answer**, not an
error: `is-active` returning 3 means "inactive". `exec` returns `nil` plus a
reason (§10) when there is no answer to give - the command could not be run,
ran out of time, or the connection failed under it.

`opts.timeout_ms` bounds the whole command: output, exit status and close.
When it passes, `exec` returns `timeout`, closes the command's channel, and
the connection stays usable for the next one:

```lua
local r, err = conn:exec("apt-get update", { timeout_ms = 120000 })
if not r and err.code == "timeout" then ... end
```

For output that should not be accumulated - a deploy log, a `tail -f`, or
anything larger than memory - take it a chunk at a time:

```lua
conn:exec("journalctl -fu app", {
    on_stdout = function(chunk) ctx.stdout:write(chunk) end,
})
```

A stream with a callback is not also accumulated, so its field comes back
empty. Without callbacks, `opts.max_output` (default 8 MiB) bounds what is
held.

`opts.stdin` writes a string to the command and then closes it - the way to
feed a command data without a shell redirect. It is capped at 128 KiB: past
roughly that, a command writing output while we write input wedges both
directions on full buffers (measured against OpenSSH). Bulk data belongs in
SFTP.

## 6a. Timeouts and keepalives

A server that accepts the connection and then goes quiet cannot hold a caller
forever; neither can one that stops answering halfway through a session.

| option (on `connect`) | default | bounds |
|---|---|---|
| `timeout_ms` | 30000 | everything up to an authenticated connection: TCP, a relay's TLS, the key exchange, userauth |
| `keepalive_ms` | 30000 | how long a read waits in silence before asking the server whether it is still there |
| `idle_ms` | 60000 | how long a connection may go with nothing at all from the server |
| `keepalive_max` | 3 | keepalives unanswered in a row before giving up; matters only with `idle_ms = 0` |

After 30 s of silence Hull sends `keepalive@openssh.com` (OpenSSH's
`ServerAliveInterval`). A server that is merely quiet - a command printing
nothing for ten minutes - answers, its answer counts as traffic, and the
connection carries on as long as the command does. A server that has gone
away answers nothing and is given up on at 60 s, with `timeout`; every later
call on that connection then fails at once with the same reason rather than
waiting again. `0` turns a bound off. Keepalives start once authenticated.

## 7. SFTP

```lua
local sftp = conn:sftp()
sftp:write("/etc/app/config.toml", contents)
local data = sftp:read("/var/log/app/today.log")
local entries = sftp:list("/var/log/app")
sftp:close()
```

Paths travel inside the subsystem as length-prefixed strings, never as words
in a command line, so a filename with a space, a quote or a `$` needs no
escaping and cannot become part of a command. SFTP also moves one direction at
a time, which is why it has no equivalent of the `stdin` cap above.

Each operation returns `nil` plus a coded reason on failure - the server's
status by name, so a missing file is `err.code == "no_such_file"` rather than
text to match:

```lua
local data, err = sftp:read("/etc/app/config.toml", 1024 * 1024)
if not data and err.code == "no_such_file" then ... end   -- `max` passed: "too_large"
local names, refused = sftp:list("/var/log/app")          -- refused: unsafe names, and why
```

## 8. Rekeying

Handled in both directions, and a caller normally sees none of it:

- A server's rekey request is absorbed wherever it arrives, including halfway
  through a streamed command.
- Hull asks for new keys once the current ones have protected about a gigabyte
  (matching OpenSSH's default `RekeyLimit`), checked between operations.

`conn:rekey()` forces one; `conn:stats()` reports what the connection has
moved and how many times it has re-keyed. Neither is needed in ordinary use.
See [`ssh_module_design.md`](ssh_module_design.md) §7 for the limits and why
there is no time-based trigger.

## 9. What is negotiated, and what is refused

| role | algorithm |
|---|---|
| key exchange | `curve25519-sha256` (also its pre-standard `@libssh.org` name) |
| host key | `ssh-ed25519` |
| encryption | `aes256-gcm@openssh.com` |
| MAC | implicit in the AEAD |
| user auth | `publickey` with `ssh-ed25519` |
| compression | `none` |

Refused by construction, not by configuration: SSH-1, `ssh-rsa`/SHA-1, DSA,
CBC modes, arcfour, MD5, DH groups 1 and 14-SHA1, and `zlib`. There is no
option to re-enable any of them, and `strict KEX`
(`kex-strict-c-v00@openssh.com`) is offered so a server that supports it gets
the stricter Terrapin-resistant rules.

`conn:negotiated()` reports what was agreed, and
`conn:fingerprint()` the host key that was accepted.

## 10. Error codes

Every method - `connect`, `exec`, `sftp`, every SFTP operation, `rekey`,
`accept_host` - returns `nil` plus a table with a `code` when it fails, so an
application branches on a value rather than on a message, and never needs
`pcall`. `detail` says more, for a person. The one thing that still raises is
an error from your own callback (an `on_stdout` that raised): that is your
bug, not a connection failure, and it comes back to you unchanged.

**Reaching the host**

| code | meaning |
|---|---|
| `denied` | the manifest refused the host, port, login or relay - and only that |
| `connect_failed` | the host did not resolve, refused, was unreachable, or TLS to the relay failed |
| `timeout` | not connected and authenticated within `timeout_ms`; or, later, the server stopped answering (see §6a) |
| `upgrade_refused` / `upgrade_failed` | the relay refused the WebSocket upgrade (carries `status`) / answered with something that is not one |
| `no_identification` / `bad_identification` | the server never sent an SSH version line / sent an unusable one |
| `host_unknown` / `host_changed` / `host_key_invalid` / `host_changed_midsession` | see §3 |
| `no_common_algorithm` / `no_kexinit_response` / `bad_kex_point` | the key exchange could not be agreed or completed |
| `service_refused` / `no_auth_response` | the server would not start user authentication / never answered it |
| `auth_failed` / `partial_success` | the key was refused, or a second factor is wanted |
| `bad_key` / `bad_passphrase` / `passphrase_required` | the key file could not be read, the passphrase was wrong, or one is needed (§4) |

**Using the connection**

| code | meaning |
|---|---|
| `channel_refused` / `no_channel_response` | the server would not open a channel, or never answered |
| `exec_refused` | the server would not run the command |
| `timeout` | `exec`'s `timeout_ms` passed; the command's channel is closed and the connection is still usable |
| `stdin_too_large` / `output_too_large` / `bad_stdin` / `bad_timeout` | a bound or an option in §6 |
| `sftp_unavailable` / `sftp_no_version` | the server has no SFTP subsystem, or it did not start |
| `no_such_file` / `permission_denied` / `failure` / `op_unsupported` / ... | an SFTP status by name; `status` holds the number |
| `too_large` | an SFTP `read` passed its `max` |
| `already_trusted` | `accept_host` for a host that already has a key; `forget_host` it first |
| `not_handshaken` | `rekey` on a connection that never finished connecting |
| `io_error` | the stream failed in some other way; `detail` says how |

`partial_success` is not success: the server wants another factor, and
reporting it as authenticated would skip one.

## 11. Not implemented

- **ssh-agent.** Needs a unix-socket (and named-pipe) capability Hull does not
  have yet; until then a key must be readable through `manifest.fs.read`.
- **Password and keyboard-interactive auth.** `publickey` only.
- **Port forwarding**, remote and local.
- **A JS implementation.** See §8 of the design record.
