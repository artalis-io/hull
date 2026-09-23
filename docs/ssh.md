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
ssh.accept_host(trust, host, err.key_blob)
```

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
            "Cf-Access-Jump-Destination: web1.internal:22",
        },
    },
}
```

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
error: `is-active` returning 3 means "inactive", and `exec` returns `nil` plus
a reason only when the command could not be run at all.

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

Every failure is a table with a `code`, so an application branches on a value
rather than on a message.

| code | meaning |
|---|---|
| `denied` | the manifest refused the host, port or login |
| `upgrade_refused` | the relay refused the WebSocket upgrade (carries `status`) |
| `host_unknown` / `host_changed` / `host_key_invalid` | see §3 |
| `no_common_algorithm` | nothing in §9 was acceptable to both ends |
| `auth_failed` / `partial_success` | the key was refused, or a second factor is wanted |
| `channel_refused` / `exec_refused` | the server would not open the channel or run the command |
| `stdin_too_large` / `output_too_large` | a bound in §6 was reached |
| `io_error` | the stream failed; `detail` carries what |

`partial_success` is not success: the server wants another factor, and
reporting it as authenticated would skip one.

## 11. Not implemented

- **ssh-agent.** Needs a unix-socket (and named-pipe) capability Hull does not
  have yet; until then a key must be readable through `manifest.fs.read`.
- **Password and keyboard-interactive auth.** `publickey` only.
- **Port forwarding**, remote and local.
- **A JS implementation.** See §8 of the design record.
