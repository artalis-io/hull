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

The store is an object with `get`/`put`/`forget`/`entries`, and Hull ships
three:

- `ssh.file_store(path)` keeps it in an **OpenSSH `known_hosts` file**, read
  and written through `hull/fs` (so the app declares `hull/fs` and names the
  file in `fs.read` and `fs.write`). The `ssh` command line reads what it
  writes and the reverse, so `ssh-keyscan web1 >> known_hosts` seeds it, and
  hashed entries (`HashKnownHosts`) are matched; `file_store(path, { hash =
  true })` writes hashed ones too. The file is read on every lookup, so a key
  changed with the `ssh` tool is seen by the next connect. Host patterns and
  `@cert-authority` / `@revoked` lines are skipped, not half-honoured: a host
  covered only by a pattern is simply unknown. A store that cannot write
  returns `store_failed`.
- `ssh.kv_store(kv)` keeps it in a **`hull.kv` namespace** (SQLite, Postgres,
  ...), for a service whose workers or instances must agree on trust.
  Accepting a key is atomic - the backend's compare-and-swap as
  set-if-absent - so two workers meeting the same new host at once cannot both
  record a key; the second gets `already_trusted`. Entries live under
  `opts.prefix` (default `"hostkey:"`), so the namespace can hold other data.
  The backend must support compare-and-swap.
- `ssh.memory_store(seed)` holds it in the process, for an app that keeps
  trust somewhere else or nowhere.

Where trust lives is still the application's decision: the manifest names the
file, or the app opens the kv namespace. The example keeps `known_hosts`
beside the app. Whoever can write the store decides which keys Hull trusts,
so treat write access to it like write access to `~/.ssh/known_hosts`.
Opening that namespace with `hull.kv`'s `encrypt` option stops a backend
writer without the key from substituting a host key; deleting one still
yields `host_unknown`, and putting back an older genuine entry is not detected
([`kv_cache.md`](kv_cache.md#encryption-at-rest)).

`host_changed_midsession` deserves its own note: a rekey re-presents the host
key, and Hull pins it against the key **this connection was built on**, not
merely against the store. A server therefore cannot present one identity at
handshake and another at rekey, even one the store would have accepted.

## 4. Keys and passphrases

`ssh.load_key(text, opts)` reads an OpenSSH private key
(`-----BEGIN OPENSSH PRIVATE KEY-----`: Ed25519, RSA, or ECDSA P-256 / P-384).
PEM / PKCS#8 keys are refused by name rather than by a generic parse error.

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

### Several keys

`keys = { ... }` in place of `key` offers them in order until the server
accepts one, as `ssh` does with several `IdentityFile` lines. Each entry is
key-file text (loaded with the connect's `passphrase` / `passphrase_env`) or a
key already loaded with `ssh.load_key` - which is how keys with different
passphrases are mixed. `conn:stats().auth_key` is the position of the one that
was accepted.

```lua
local conn, err = ssh.connect{
    host = "web1", user = "deploy",
    keys = { fs.read("id_ed25519"), ssh.load_key(fs.read("id_rsa"),
                                                 { passphrase_env = "RSA_PASS" }) },
}
```

- Every key is loaded before anything is dialled: a damaged one is
  `bad_key` (with `err.key`, its position) rather than a refusal later.
- An RSA key is still tried under SHA-512 and then SHA-256 before the next
  key.
- The next key is offered only after a plain refusal that still lists
  `publickey`. `partial_success` stops at the key that earned it - the server
  wants a second factor, which another key does not supply.
- When none is accepted the error is one `auth_failed`, with
  `detail = "none of the N keys was accepted"`.
- The server's limit on attempts still applies: OpenSSH's `MaxAuthTries`
  (6 by default) counts every signature, and an RSA key spends two.

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

`opts.stdin` feeds the command data without a shell redirect, then closes
its input. Two forms:

- **A string** is written whole. It is capped at 128 KiB (`stdin_too_large`,
  before anything is sent): past roughly that, a command writing output while
  we wrote input wedged both directions on full buffers, measured against
  OpenSSH on Windows.
- **A function** is a source, called for the next chunk until it returns
  `nil` or `""` - no cap. Between writes Hull takes in whatever output the
  command has already sent (and reopens its window), so input and output
  move together; add `on_stdout` to stream both ways. A source that raises
  raises out of `exec`; one returning a non-string is `bad_stdin`.

```lua
local left = 80
conn:exec("sha256sum", {
    stdin = function()                        -- 5 MB, 64 KiB at a time
        if left == 0 then return nil end
        left = left - 1
        return string.rep("x", 65536)
    end,
    timeout_ms = 60000,
})
```

A server that stops reading its command's input (the wedge above) is still
bounded only by `timeout_ms`, so set one. Files belong in SFTP, which moves
one direction at a time.

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

## 6b. Several connections at once

A connection belongs to the task that opened it, and one task drives it at a
time. Hull runs each HTTP request handler and each timer callback
(`app.every` / `app.daily`) as its own task on the event loop, and
`app.main` as one more; a connection waiting on the network parks its task and
lets the others run. So:

- **Many connections, one per task, run concurrently.** Two requests that each
  connect to a different host are both in flight at once.
- **One connection used from two tasks is refused**, not queued: the second
  gets `busy: another coroutine is waiting on this connection`. Share a
  connection only between calls in the same task - an `exec` and an open SFTP
  session on one connection are fine, one after the other.
- **`app.main` is one task, so its connections run one after another.** There
  is no primitive yet for starting further tasks from `app.main`, and a
  coroutine the app creates itself (`coroutine.create`) is not a task: SSH
  calls must not be made from one. A fleet tool that reaches ten hosts from
  `app.main` reaches them in turn; each host's share is bounded by the
  connect `timeout_ms` and the idle bounds in §6a, so one unreachable host
  delays the rest by at most those bounds.

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

A session waits at most `reply_timeout_ms` (default 60000; `0` for no bound)
for the server's next message: `conn:sftp({ reply_timeout_ms = 30000 })`. Any
message for the session resets it, so a large transfer that keeps moving is
never cut off. What it catches is a server that stays connected - still
answering keepalives, so the connection's idle bound (§6a) never fires - but
stops answering the session. That call returns `timeout`, the session is
closed (replies to what it already sent could still arrive, so it is not
reused), and every later call on it returns `timeout` too.

An sftp session and commands can share a connection. While `exec` runs, what
arrives for the open sftp session - data, a window adjust, a keepalive request
- is applied to the session (a request needing a reply is answered at once)
and its data kept for its next call; the same holds the other way round.

Each operation returns `nil` plus a coded reason on failure - the server's
status by name, so a missing file is `err.code == "no_such_file"` rather than
text to match:

```lua
local data, err = sftp:read("/etc/app/config.toml", 1024 * 1024)  -- max; default 16 MiB
if not data and err.code == "no_such_file" then ... end   -- `max` passed: "too_large"
local names, refused = sftp:list("/var/log/app")          -- refused: unsafe names, and why
```

The rest of the file operations:

| method | does |
|---|---|
| `stat(path)` / `lstat(path)` | `{ size, uid, gid, permissions, atime, mtime, is_dir }` (lstat does not follow a symlink) |
| `mkdir(path, mode?)` / `rmdir(path)` / `remove(path)` | create a directory, remove an empty one, remove a file |
| `rename(from, to)` | SFTP v3 refuses when `to` exists (`failure`); remove it first to replace |
| `chmod(path, mode)` / `setstat(path, attrs)` | permissions, or any of `size`, `uid`+`gid`, `atime`+`mtime` |

Modes are octal **strings** - `"755"`, not `755`, which Lua reads as decimal.

A deploy step that must never leave a half-written file in place writes
beside it and renames:

```lua
assert(sftp:write("/srv/app/config.toml.new", contents))
assert(sftp:chmod("/srv/app/config.toml.new", "640"))
sftp:remove("/srv/app/config.toml")                     -- rename will not replace
assert(sftp:rename("/srv/app/config.toml.new", "/srv/app/config.toml"))
```

### Files larger than memory

`read` and `write` move a whole file. For one too large to hold, or to read
part of one, open it:

```lua
local f = assert(sftp:open("/var/log/app/big.log", "r"))
while true do
    local chunk, err = f:read(1024 * 1024)       -- up to 1 MiB at a time
    if not chunk then error(err.code) end
    if chunk == "" then break end                -- end of file
    ctx.stdout:write(chunk)
end
f:close()
```

Modes are `"r"`, `"r+"`, `"w"` (create or truncate), `"a"` (append) and
`"wx"` (create; `failure` if it exists); `open(path, "w", { mode = "600" })`
sets a new file's permissions. A file has `read(n)`, `write(data)`,
`seek(offset)`, `tell()`, `stat()` and `close()`. Close it: an open file stays
open on the server until it is closed or the SFTP session is.

Large transfers are pipelined: requests go out 32 KiB at a time with up to 64
in flight (what OpenSSH's `sftp` keeps), as a sliding window - a new request
goes out as each reply comes back, so the pipe never drains between batches.
A big file over a relay moves at the link's speed rather than one round trip
per 32 KiB. A read starts with one request and grows the window by one per
full reply, so a small file costs no more than it would otherwise. A READ
reply longer than the request is refused (`bad_reply`).

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
| host key | `ssh-ed25519`, then `ecdsa-sha2-nistp256`, `ecdsa-sha2-nistp384`, `rsa-sha2-512`, `rsa-sha2-256` |
| encryption | `aes256-gcm@openssh.com`, then `chacha20-poly1305@openssh.com` (only under strict KEX) |
| MAC | implicit in the AEAD |
| user auth | `publickey` with `ssh-ed25519`, `ecdsa-sha2-nistp256` / `-nistp384`, or an RSA key under `rsa-sha2-512` then `rsa-sha2-256` |
| compression | `none` |

`chacha20-poly1305@openssh.com` is for servers that disable AES-GCM. It is
negotiated only when strict KEX is in effect: its nonce is the packet sequence
number, which is exactly what the Terrapin attack (CVE-2023-48795) shifts, so
against a server without the `kex-strict-s` marker Hull keeps it out of the
offer. AES-256-GCM stays first either way (hardware AES where the CPU has it).
Force one with `offer.cipher` on `connect`; `conn:negotiated().cipher_c2s`
says which was agreed.

Host keys are offered in that order, with one exception: when the trust store
already holds a key for the host, that key's type goes first. A server with
several host keys presents the one the client ranks highest, so a host known
by its RSA key would otherwise present Ed25519 and read as a changed key.
OpenSSH orders its offer the same way. ECDSA and RSA host keys are checked
with `crypto.verify` (P-256 / P-384 with SHA-256 / SHA-384; RSA PKCS#1 v1.5
with SHA-512 or SHA-256), always under the algorithm that was negotiated. An
RSA host key must be at least 2048 bits. `ecdsa-sha2-nistp521` is not offered.
`file_store` trusts a recorded key of any of these types; if a host has
several, Ed25519 wins, then ECDSA, then RSA.

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
| `host_revoked` | the trust store revokes the key the server presented (a known_hosts `@revoked` line) |
| `store_failed` | the trust store could not be read or written (a missing known_hosts file is an empty store, not this) |
| `no_common_algorithm` / `no_kexinit_response` / `bad_kex_point` / `unexpected_message` | the key exchange could not be agreed or completed |
| `service_refused` / `no_auth_response` | the server would not start user authentication / never answered it |
| `auth_failed` / `partial_success` | the key (or every key in `keys`) was refused, or a second factor is wanted |
| `bad_key` / `bad_passphrase` / `passphrase_required` | the key file could not be read, the passphrase was wrong, or one is needed (§4); with `keys`, `err.key` is the position |
| `unsupported_key_type` | the key is not an OpenSSH Ed25519, RSA or ECDSA P-256/P-384 key (P-521, a PEM file), is an RSA key under 2048 bits, or uses protection Hull does not read |

**Using the connection**

| code | meaning |
|---|---|
| `channel_refused` / `no_channel_response` | the server would not open a channel, or never answered |
| `exec_refused` | the server would not run the command |
| `timeout` | `exec`'s `timeout_ms` passed; the command's channel is closed and the connection is still usable. Or an sftp session's `reply_timeout_ms` passed (§7); that session is closed |
| `stdin_too_large` / `output_too_large` / `bad_stdin` / `bad_timeout` | a bound or an option in §6 |
| `sftp_unavailable` / `sftp_no_version` | the server has no SFTP subsystem, or it did not start |
| `no_such_file` / `permission_denied` / `failure` / `op_unsupported` / ... | an SFTP status by name; `status` holds the number |
| `closed` | an SFTP file handle used after `close` |
| `bad_argument` | an option out of range, e.g. `sftp{ reply_timeout_ms = -1 }` |
| `too_large` | an SFTP `read` passed its `max`, or `list` found more than `MAX_LIST_ENTRIES` (100000) entries |
| `bad_reply` | the SFTP server answered with a message that does not fit the request |
| `bad_software` | `connect`'s `software` option is not a valid identification string (letters, digits, `.`, `_`) |
| `already_trusted` | `accept_host` for a host that already has a key; `forget_host` it first |
| `host_revoked` / `host_key_invalid` | `accept_host` for a key the store revokes / a blob that is not a host key Hull verifies (Ed25519, ECDSA P-256/384, RSA) |
| `not_handshaken` | `rekey` on a connection that never finished connecting |
| `io_error` | the stream failed in some other way; `detail` says how |

`partial_success` is not success: the server wants another factor, and
reporting it as authenticated would skip one.

## 11. Not implemented

- **ssh-agent.** Needs a unix-socket (and named-pipe) capability Hull does not
  have yet; until then a key must be readable through `manifest.fs.read`.
- **Password and keyboard-interactive auth.** `publickey` only.
- **ECDSA P-521 keys**, host or user: `hull.crypto` has no ES512. P-256 and
  P-384 are supported; a P-521 key file is refused by name
  (`unsupported_key_type`).
- **Port forwarding**, remote and local, and **jump hosts** (`ProxyJump`).
  A WebSocket relay (§5) is the supported way to reach a host behind another.
- **A PTY or an interactive shell**, and setting environment variables on the
  remote side. `exec` runs one command without a terminal.
- **Certificates**, for host or user keys. A `known_hosts` file's
  `@cert-authority` lines are skipped (§3).
- **A JS implementation.** See §8 of the design record.
