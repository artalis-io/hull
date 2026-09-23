# hull/ssh Phase 0 design report

**Status: decisions taken; phase 1 unblocked.** See section 12 for the record.
The SSH protocol work is viable in pure Lua/JS, and almost every primitive it
needs already exists. Two do not, and both are generic capabilities that belong
to Hull rather than to SSH. This report identifies them, proposes the smallest
general APIs that would satisfy them, and stops there, per the native-code
policy.

Driver: `hsctl`, a cross-platform fleet controller for DGX Spark nodes.

## 1. Verdict first

| question | answer |
|---|---|
| Can the SSH protocol live in Lua/JS? | Yes. Framing, KEX orchestration, auth, channels and SFTP are all byte manipulation and state machines. |
| Does Hull expose raw outbound TCP to stdlib code? | **No.** This is the blocker. |
| Can Keel do it? | **Yes, already.** `kl_connect_op_*` plus `KlStream`, used today by `cap/smtp_transport.c`. |
| Are the crypto primitives present? | Mostly. Three are missing, all small, all already implemented inside vendored code. |
| Is new native C required? | Yes, but **not for SSH**. For one generic stream capability that Hull is missing on its own merits. |

The honest framing: SSH is acting here exactly as the task intended, as a design
probe. What it found is that **Hull can serve the network but cannot dial it**
from stdlib code, except through three hard-coded protocol clients (HTTP, SMTP,
WebSocket) each of which privately re-implements the same connect-and-stream
dance in C.

## 2. Repository alignment

### 2.1 Proposed package path

```
stdlib/lua/hull/ssh/         Lua implementation (init.lua + submodules)
stdlib/js/hull/ssh/          JS binding or implementation (see section 8)
docs/ (user guide)           user-facing documentation, added in phase 5
stdlib/lua/hull/tests/test_ssh_*.lua    co-located suites
```

Registry name: `hull/ssh`, declared as `"hull/ssh@1"` in `manifest.modules`.

**Why this path.** Hull's registry (`src/hull/module_registry.c`) is flat under
`hull/`, with `hull/web/*` reserved for strictly-web modules and cross-cutting
facilities staying at the top level (`hull/jwt`, `hull/smtp`, `hull/http-client`,
`hull/template`). SSH is cross-cutting infrastructure, not web, so it sits at
the top level beside `hull/smtp`. Multi-file modules already use a directory
(`hull/web/htmx/*`, `hull/crypto/envelope`), so a `ssh/` subtree with an entry
module matches existing practice and avoids the monolithic-file problem.

Explicitly NOT `tools/` or an `hsctl`-specific subtree, per the brief.

### 2.2 Closest analogue, and an important asymmetry

The closest analogue by *purpose* is `hull/smtp`: an outbound, authenticated,
stateful network client with TLS and a strict error model. `docs/smtp_keel_client_design.md`
section 4 is the single most relevant prior art in the repo.

But there is an asymmetry worth naming up front, because it shapes everything:

| module | implementation | has a `.lua` file? |
|---|---|---|
| `hull/http-client`, `hull/smtp`, `hull/web/ws-client` | C cap | **no** |
| `hull/web/pwned`, `hull/email`, `hull/retry`, `hull/cache` | pure Lua/JS | yes |

**Every module with network authority today is C. Every pure-Lua module reaches
the network only by calling one of those C caps.** `hull/ssh` as proposed would
be the first pure-Lua module to drive a raw socket. That is architecturally
sound and is what the brief asks for, but it is a new combination, and it is
exactly why the missing piece below is a generic capability rather than an SSH
detail.

## 3. Dependency classification

| SSH need | Existing Hull facility | Sufficient? | Action |
|---|---|---:|---|
| TCP byte stream (connect/read/write/close) | none public; `cap/smtp_transport.c` privately over `KlConnectOp` + `KlStream` | **no** | **propose `hull/net` stream capability** |
| CSPRNG | `crypto.random` | yes | reuse |
| SHA-256 | `crypto.sha256`, `crypto.create_sha256` (streaming) | yes | reuse |
| HMAC-SHA256 | `crypto.hmac_sha256`, `hmac_sha256_verify` | yes | reuse |
| Ed25519 sign/verify | `crypto.ed25519_sign` / `_verify` / `_keypair` | yes | reuse |
| Constant-time compare | `crypto.constant_time_eq` | yes | reuse |
| X25519 ECDH (curve25519-sha256 KEX) | TweetNaCl `crypto_scalarmult` is vendored and linked; cap uses only `crypto_scalarmult_base` (`cap/crypto.c:1727`) | **no** | **propose `crypto.x25519`** |
| AEAD: chacha20-poly1305 or aes256-gcm | neither exposed; `secretbox` is XSalsa20-Poly1305, wrong construction | **no** | **propose one AEAD (see 5.2)** |
| Byte cursor / endian codec | Lua 5.4 `string.pack`/`unpack` (string lib is loaded); JS `DataView` | yes | reuse |
| Byte-safe strings / buffers | Lua strings are byte-clean; JS `ArrayBuffer`; unified buffer protocol for zero-copy | yes | reuse |
| Async / event loop | `hull.sleep`, coroutine yield; `db.async`, `compute.async` precedent | partial | reuse; see 5.1 for stream readiness |
| Deadlines | `app.every` / `app.daily` timers, `time.now_ms` | yes | reuse |
| Cancellation | per-op in C caps; nothing script-visible for a stream | **no** | folds into the `hull/net` proposal |
| Secure zeroization | `hull_secure_zero` is C-internal; no script binding | **no** (minor) | see 5.3, may be deferred |
| Capability host/port gate | `hl_host_match_any_env` shared matcher; `databases.dynamic` / `kv.dynamic` manifest precedent | yes | **extend, do not invent** |
| Structured errors | stdlib convention: coded errors (`email.send` throws `.code`) | yes | reuse |
| SQLite trust store | `hull/db` | yes | reuse, app-owned |

Ten of fourteen reuse cleanly. The four `no` rows collapse into **two
proposals**, because cancellation and deadlines are properties of the stream
API rather than separate facilities.

## 4. The blocker, stated precisely

Hull has no public outbound byte-stream capability. The runtime is fully capable
of one:

```
kl_connect_op_init / _start / _cancel      async connect with resolve,
kl_connect_op_on_resolved / _on_attempt_*  IPv4+IPv6 racing, delay, deadline
KlStream                                   ordered bounded writes, read delivery,
                                           backpressure, cancel
```

`src/hull/cap/smtp_transport.c` already composes exactly these. The contract it
had to establish is written down in `docs/smtp_keel_client_design.md` section 4
(Open / Byte stream / TLS upgrade / Ownership) and reads like a specification
for the generic facility that does not exist.

So the situation is not "Hull cannot do this". It is "Hull does this privately,
once per protocol, in C". HTTP, SMTP and WebSocket each pay that cost. SSH would
be the fourth, and the first that could avoid it if the facility were public.

**Per the hard rule, I am not implementing sockets inside `hull/ssh`.**

## 5. Proposed stdlib additions

### 5.1 `hull/net`: outbound byte stream capability

The smallest general API that satisfies SSH and the three existing clients.

```lua
local net = require("hull.net")

local s, err = net.connect({
    host = "spark-7468.local",
    port = 22,
    timeout_ms = 10000,
})                          -- yields; capability-checked BEFORE any DNS or TCP

local n   = s:write(bytes)  -- bounded, all-or-none admission
local buf = s:read(4096, { timeout_ms = 5000 })   -- nil on clean EOF
s:close()                   -- graceful; s:cancel() for abortive
```

- Manifest-gated (section 6). The check happens before resolution.
- Async by the same coroutine-yield model as `db.async` and `http.fetch`; no
  busy-wait, no second event loop.
- Deadline and cancellation are stream properties, so SSH inherits both.
- TLS upgrade is deliberately **out of scope for SSH** (SSH does its own
  crypto), but the same object is the natural place for a later `s:start_tls()`
  that SMTP's STARTTLS path could eventually share.

This is native C, and it is the one piece I will not write without explicit
approval. It is justified under the native-code policy on all four counts:
it is below the stdlib boundary, it cannot be expressed with existing public
facilities, the correct fix is generic rather than SSH-shaped, and it is being
presented before implementation.

**Sizing:** the logic already exists in `cap/smtp_transport.c`; the work is
generalizing it and adding script bindings, not writing a new transport.

### 5.2 `crypto.x25519(scalar, point)`

Raw X25519 scalar multiplication, for `curve25519-sha256` key exchange.

TweetNaCl's `crypto_scalarmult` is **already vendored, already compiled, already
linked**. `cap/crypto.c` calls its `_base` variant today to generate box
keypairs. This exposes the two-argument form.

This is the smallest possible addition: one binding over an existing linked
function. It is generally useful (any ECDH protocol needs it), not SSH-specific.

### 5.3 One AEAD

SSH needs authenticated encryption for the transport. Neither candidate is
available:

| candidate | status | cost |
|---|---|---|
| `chacha20-poly1305@openssh.com` | absent. `secretbox` is XSalsa20-Poly1305, a different construction with a fixed nonce layout; not reusable | needs raw ChaCha20 + Poly1305 exposed separately, because OpenSSH uses two keys and encrypts the length field independently |
| `aes256-gcm@openssh.com` | absent from the cap layer. mbedTLS is vendored and has GCM, but no Hull code references `mbedtls_gcm` | thinner: one binding over an existing vendored implementation, but only in TLS-linked builds |

**Recommendation: `aes256-gcm@openssh.com`**, exposed as a generic
`crypto.aes256gcm_encrypt/decrypt`. It is a smaller addition (mbedTLS already
implements it), it is generically useful, and it avoids exposing raw stream-cipher
and one-time-MAC primitives that are easy to misuse outside SSH's specific
construction. The trade-off is that it is only available where mbedTLS is linked,
which is every build with either HTTP half on, but not a `pure-compute` flavor.

I would rather be told which of these two you prefer than pick silently, because
the choice has a build-flavor consequence.

## 6. Capability schema

Follows `databases.dynamic` and `kv.dynamic` exactly, including fail-closed
semantics and the shared `hl_host_match_any_env` matcher (exact host, `*`,
`*.suffix` glob, CIDR for IP literals, `$VAR` env refs).

```lua
app.manifest({
    modules = { "hull/ssh@1" },
    ssh = {
        connect = {
            hosts = { "*.local", "10.0.0.0/8", "$SPARK_HOST" },
            ports = { 22 },              -- default { 22 } if omitted
            users = { "operator" },      -- optional; omitted means any
        },
    },
})
```

Properties, all matching existing Hull behaviour:

- No `ssh` block, or an empty one, denies every connection. Fail closed.
- Declaring `hull/ssh@1` grants **module resolution only**, never authority.
  This is the `hull/http-client` model verbatim: the import succeeds, the call
  is what gets refused.
- The check runs **before** DNS resolution and before any socket exists.
- Denial is a distinct structured error (`capability_denied`), never a timeout
  or a generic failure.
- `hull/ssh` must NOT grant `hull/net` authority transitively. An app that
  declares SSH can reach port 22 on allowlisted hosts and nothing else.

That last point matters and is worth a test: if `hull/ssh` is built on
`hull/net`, the SSH capability has to be a *narrower* grant, not a re-export.

## 7. Algorithm set

Deliberately minimal. Interoperability with current OpenSSH, not legacy breadth.

| role | algorithm | basis |
|---|---|---|
| KEX | `curve25519-sha256` | needs 5.2 |
| host key | `ssh-ed25519` | `crypto.ed25519_verify`, present |
| encryption | `aes256-gcm@openssh.com` | needs 5.3 |
| MAC | implicit in AEAD | none needed |
| user auth | `publickey` with `ssh-ed25519` | `crypto.ed25519_sign`, present |
| compression | `none` only, explicitly rejected otherwise | |

Rejected by construction: SSH-1, ssh-rsa/SHA-1, DSA, CBC modes, arcfour, MD5,
DH groups 1/14-SHA1, `zlib`.

Rekey: implemented if it is cheap once the transport exists; otherwise a hard
connection byte/time limit with a documented, enforced ceiling, never a silent
overrun.

## 8. Lua and JS: an open architectural question

**Hull has no shared-implementation mechanism between Lua and JS.** Every stdlib
module is implemented twice, in parallel files (`stdlib/lua/hull/X.lua` and
`stdlib/js/hull/X.js`), kept at parity by convention and by the co-located test
suites. The only genuinely shared implementation surface in Hull is WASM
compute, which has no I/O and so cannot host a protocol driver.

An SSH-2 client is on the order of several thousand lines. Implementing it twice
contradicts the brief's "do not duplicate the SSH protocol implementation", but
implementing it once contradicts Hull's stdlib parity convention. This is the
one genuine architectural ambiguity in the report, and section 23 of the brief
says to stop for review on exactly that.

Options, with my assessment:

1. **Lua first, JS binding later.** Lua is already Hull's privileged tooling
   language (the entire CLI tool layer is Lua-only by design, per CLAUDE.md).
   `hsctl` can be a Lua app. Ships soonest, defers the parity debt honestly.
   **My recommendation.**
2. **Both from the start.** Doubles the protocol surface, and doubles the
   security-review surface, which is the expensive part.
3. **Protocol core as a WASM compute module**, pure transform, with I/O in the
   host language. Genuinely shared, and the crypto could come along. But it
   inverts Hull's own layering (stdlib depending on a compute artifact), needs a
   build-pipeline story for shipping a `.wasm` inside stdlib, and adds a
   toolchain dependency to a security-critical path. Interesting, and I do not
   recommend it for v1.

## 9. Error model

Structured codes, following the `email.send` convention of a coded error rather
than a string. Distinguishing at minimum:

```
capability_denied     connect_failed        timeout
protocol_error        kex_failed            algorithm_mismatch
hostkey_rejected      hostkey_changed       auth_failed
channel_rejected      transfer_failed       connection_closed
```

`exec` returning a non-zero remote exit status is **not** an error. It is a
successful call whose `exit_status` field is non-zero, exactly as the brief
requires.

### 9a. exec delivers output two ways

Accumulating a command's output and returning it whole is convenient for
`uname -a` and wrong for everything else: the caller sees nothing until the
process exits, and the output has to fit in memory. So each stream is
delivered one of two ways, chosen per stream:

```lua
conn:exec("uname -a")                          -- accumulate; r.stdout
conn:exec("journalctl -fu app",                -- stream; r.stdout stays empty
          { on_stdout = function(chunk) ... end })
```

A stream with a callback is never also accumulated, so `max_output` (8 MiB by
default) bounds only the accumulating path. `opts.stdin` is written to the
command and closed before its output is drained, which is how a command is fed
data without a shell redirect - the same reason SFTP exists rather than
`cat > file`.

### 9b. Why `exec` bounds stdin

`opts.stdin` is capped at 128 KiB, and a larger one is refused up front with
`stdin_too_large` rather than written.

The reason is measured, against OpenSSH on Windows over loopback:

| stdin | stdout | result |
|---|---|---|
| 122 KiB | 131 KiB | completes |
| 305 KiB | 330 KiB | stalls until the socket times out |
| 1.2 MiB | a single line | instant |

So the wall is the two directions **together** - around 256 KiB, four 64 KiB
socket buffers - not the size of either one.

The obvious fix is to interleave reads with the writes, and it does not work.
Instrumented against that server with a `select`-backed readiness probe,
**nothing was readable at any point during the write** (18 of 19 probes false):
the peer has stopped producing, so there is nothing for a client to drain and
no scheduling change relieves it. Only writing less does. An earlier revision
of this design added an optional `stream:readable()` for exactly that fix; it
was removed once the measurement showed it did not deliver one.

Nor can a client read on a fixed schedule instead: a command like
`find /c /v ""` emits nothing until its input closes, so a client that stops
to read mid-write deadlocks the other way round.

A bound therefore is the fix, not a workaround for a missing one. It costs the
1.2 MiB-quiet case, which is bulk data, which belongs in SFTP - one direction
at a time, and no such limit.

## 10. Host key exposure

The host key object is available to the caller before any trust decision, and
carries raw canonical bytes, not only a formatted fingerprint:

```lua
{
    algorithm   = "ssh-ed25519",
    raw         = "<32 bytes, canonical SSH wire encoding>",
    fingerprint = "SHA256:abc...",     -- convenience only
}
```

`raw` is what `hsctl` binds into its enrollment HMAC. There is no default-accept
path: `host_key` is a **required** field, and returning anything other than an
explicit accept rejects the connection. No global `known_hosts` is read or
written; trust storage is the application's, in its own SQLite.

## 11. Phasing

Phase 0 (this document) stops here pending decisions.

| phase | content | gate |
|---|---|---|
| 0 | this report | **your approval of 5.1 and 5.3** |
| 1a | `hull/net` stream capability + tests | native C, needs approval |
| 1b | `crypto.x25519` + chosen AEAD + test vectors | native C, needs approval |
| 2 | codec, identification, KEX, encrypted transport, host-key exposure | pure Lua |
| 3 | Ed25519 user auth, session channel, exec, streams, exit status | pure Lua |
| 4 | SFTP subset (upload/download, no shell quoting) | pure Lua |
| 5 | fault injection, fuzz targets, capability-denial tests, docs | |

Phases 2 to 5 are pure Lua/JS and need no further approval.

## 11a. Where the protocol lives, and why not C

Revisited deliberately once the shape of `hull/net` became clear, because the
honest case for a C implementation had grown: every other network protocol in
Hull is C (`cap/http.c`, `cap/ws.c`, `cap/smtp.c`, `cap/pgwire.c`,
`cap/mysqlwire.c`), a C cap serves Lua and JS from one implementation, and the
byte-stream capability would have had exactly one consumer.

**Decision: the SSH protocol is implemented in Lua.** The reasoning that
settled it:

**Consistency is a description of history, not an argument.** That pgwire is C
says how Hull got here, not where it should go. `hull/ssh` is a deliberate test
of whether Hull can stop growing protocol-specific native code.

**The risk profile is not comparable.** Before authentication, SSH parses and
maintains state over attacker-controlled traffic across identification,
KEXINIT, algorithm negotiation, X25519, host-key parsing, key derivation,
encrypted packet framing, AEAD, sequence numbers, service negotiation, userauth,
channels, window management and rekey. That is a large stateful hostile-input
surface, and it is the case Hull's own thesis is about: push memory-unsafe code
down into small auditable primitives and keep protocol logic in a safe language.

**The JS argument proves too much.** A C implementation gives both runtimes
from one source, and Hull has no mechanism for a JS module to consume a Lua
stdlib implementation. But if parity forces C, then EVERY substantial stdlib
module is forced to C, and the orchestration model becomes "C for anything
reusable". That is a larger architectural question than SSH, and SSH should not
decide it by accident. Recorded as an open question instead (section 12a).

**The native-code rule, sharpened.** The original brief said "preferably no
native C", which is vague about what the exception covers. The rule this design
now follows:

> Protocol semantics must not move into native C merely because a stdlib
> primitive is missing. Native C is permitted only for small generic primitives
> below the safe-language boundary.

That permits `cap/net_stream.c`, a crypto primitive, an event-loop bridge. It
forbids a `cap/ssh.c` containing the SSH parser and state machine, unless that
decision is revisited explicitly rather than arrived at by drift.

## 12a. Open: language-neutral stdlib implementations

`hull/ssh` is Lua-only for v1, so `hsctl` is Lua. That is acceptable now and
unsatisfying later, and the general problem is not SSH's to solve:

> Should Hull have a way to implement a stdlib module once and bind it to both
> runtimes?

Today the answer is no: every stdlib module is written twice, and the only
shared implementation surface is WASM compute, which has no I/O. Options worth
examining when this is picked up deliberately include a shared implementation
compiled to WASM with thin per-runtime bindings, or accepting per-runtime
implementations as the permanent model and tooling the parity checks.

Until then, a Lua-only module is a recorded gap, not a reason to move protocol
logic into C.

## 11b. Reaching a host through a tunnel

Shipped after phase 5, and worth recording because it touched the capability
schema rather than only the protocol.

A host behind a WebSocket-over-TLS relay (Cloudflare Access is the case that
drove it) is reached with `ssh.connect{ tunnel = {...} }`. Three facts made it
smaller than it looks:

1. **The SSH code did not change.** `hull.ssh.transport` takes a stream with
   `read`/`write`/`close`. `hull.web.ws-stream` turns a stream carrying a
   WebSocket into a stream carrying the bytes inside it, which is the same
   interface. So the tunnel is a different `open_stream` and nothing else.
2. **The WebSocket client holds no authority.** It is handed its stream, its
   RNG and its digest, so it can only transform a connection someone else
   opened. It knows nothing about SSH and nothing about Cloudflare; the
   provider's `Cf-Access-*` values are just headers the caller passes.
3. **TLS was already the transport's job**, once `cap/net_stream.c` grew a
   `KlTlsConfig`. The binding only had to be able to ask for it.

### The grant is two grants

The one genuinely new decision. `hl_ssh_check_connect` is handed the SSH
destination, and through a tunnel the destination is not what is dialled.
Neither obvious answer works:

| option | why not |
|---|---|
| gate only the relay | the destination travels inside the tunnel's headers and never appears in the socket address, so `hosts = {"ssh.example.com"}, ports = {443}` would authorise SSH to EVERY host behind that edge |
| gate only the destination | a socket is opened to a machine the manifest never named, and that machine terminates the TLS and sees the tunnel credentials |

So both, as separate keys:

```lua
ssh = {
    connect = { hosts = {"spark-7468"}, ports = {22}, users = {"operator"} },
    tunnel  = { hosts = {"ssh.example.com"}, ports = {443} },
}
```

`connect` keeps its existing meaning exactly - which machine may be reached
and as which login - so adding a tunnel widens neither. `tunnel` says which
relay may be dialled to get there, and has no `users`: the login belongs to
the target, and the relay never sees it. Both fail closed (absent or empty
grants nothing), both go through the shared `hl_host_match_any_env` matcher,
and both are sealed. The denial reasons are separate values, because a message
that says "host is not in ssh.connect.hosts" when the RELAY was refused sends
the reader to the wrong list.

### Two adjacent bugs this surfaced

Neither was about tunnels; both made an SSH-only app fail in the mode it is
for, and both had been latent since `hull/ssh` landed.

- **The kernel sandbox never granted `network_outbound` for ssh.** It was
  granted for `hosts` or a declared network DB. But `hull/ssh` deliberately
  does not take `HL_MOD_CAP_HOSTS` - `ssh.connect` is its own grant - so a
  fleet tool declaring only `ssh` declared no hosts and was SIGKILLed on
  connect. The same mistake the `databases` clause beside it already records.
- **The CA bundle was resolved inside the `hosts` block**, which made the
  outbound trust anchor a property of `http.fetch`. An app declaring only
  `ssh` had none and no way to ask for one. It is resolved once now, on both
  entry paths, and handed to the runtime as `HlClientTls`, so `--ca-bundle`,
  `--no-ca-bundle` and the system/embedded ladder mean one thing for every
  outbound connection Hull makes.

### How it is tested

Three layers, because the interesting failures are at different ones:

| layer | what it drives | where |
|---|---|---|
| unit, Lua | `ssh.connect` against a FAKE relay - the upgrade request byte for byte, `upgrade_refused` vs `denied`, a peer that is not a WebSocket server | `stdlib/lua/hull/tests/test_ssh_tunnel.lua` |
| unit, C | the two grants in isolation, with no socket, resolver or loop in scope | `tests/hull/cap/test_net_policy.c` |
| e2e | a REAL WebSocket relay (`tests/fixtures/ws_tcp_shim.py`) over real sockets, in front of a real OpenSSH `sshd` | `tests/e2e_ssh_tunnel.sh`, `make e2e-ssh-tunnel` |

The e2e has two parts. The SEAM checks need only python3: the upgrade over a
real socket, the caller's headers arriving verbatim, an Access-style 403
reported as `upgrade_refused`, and both grants refusing. The LIVE SESSION
puts an actual `sshd` behind the relay and runs the whole thing - curve25519
/ ed25519 / aes256-gcm, publickey auth, `exec`, exit status, and the host-key
accept plus reconnect as a second real tunnel.

The live session skips where sshd is absent, which is right on a laptop and
wrong in CI, so `HULL_E2E_REQUIRE_SSHD=1` turns the skip into a failure and
ci.yml sets it. A job that installs openssh-server and then quietly tests
nothing is worse than a red one.

### Where the trust anchor comes from

`serve_cli.c` used to resolve its anchor from the embedded Mozilla bundle
ALONE, honouring neither `--ca-bundle` nor `--no-ca-bundle` - both of which
`serve.c` already did. A public-CA relay (what Cloudflare is) worked; a relay
behind an internal CA was unreachable, and `--ca-bundle` looked accepted while
doing nothing. An `app.main` program is exactly what a fleet tool is, so this
was the one entry point where it mattered most.

Both now walk the same five rungs: `--no-ca-bundle`, then `--ca-bundle PATH`,
then the system store, then the embedded bundle, then a warning naming all
three ways out. The system-store probe moved into `cacert.c` beside the
embedded one rather than being copied - a second copy of a path walker is how
`hull doctor` and `hull tools list` once disagreed about the same tool on the
same box.

So the e2e now drives the tunnel over TLS too. It mints a private CA, puts the
relay behind a certificate it signed, and checks the rung that carries the
whole guarantee: that the DEFAULT anchor REFUSES that certificate, and only
`--ca-bundle` makes it trust that one. A TLS test that never sees a refusal is
not testing verification.

**What the e2e found on its first run**, which is the argument for having it:
`hull/ssh` could not connect AT ALL, on any platform. Two defects, both
invisible to every unit suite. `ssh_park` never registered the stream's op
with the async backend, so the backend had nothing to resume; and both async
backends freed an op's state AFTER running its callback, clobbering the state
a re-suspend had just installed. No other consumer re-enters - `http.fetch`,
`compute.async` and `gpu.async` park once per operation - while a byte stream
parks on every read and every write. `test_net_stream` missed it because it
pumps the loop itself and calls `hl_net_stream_connect_result` directly,
which is the transport's contract, not the binding's.

### Still open

- No worked example under `examples/`, and no user-facing guide; this design
  record is still the only documentation.
- JS remains unimplemented, per section 8 - `hull.web.ws-stream` is Lua-only
  too, so a JS `hull/ssh` inherits this work rather than duplicating it.

## 11c. Passphrase-protected keys

`hull/ssh` originally refused every encrypted key, which meant it worked on
exactly the keys a fleet tool does not meet. It now reads what `ssh-keygen`
writes by default: `aes256-ctr` under the `bcrypt` KDF.

### The passphrase is named, not carried

The design constraint is a property of the runtime, not of SSH. **Lua strings
are immutable and interned**: one cannot be overwritten in place, and it stays
in the script heap until collection, on memory that is not scrubbed when
freed. A passphrase that becomes a Lua value is therefore a passphrase Hull
cannot clean up.

So the preferred route never lets it become one:

```lua
ssh.connect{ host = ..., user = ..., key = text,
             passphrase_env = "SPARK_KEY_PASSPHRASE" }
```

Lua passes the NAME. The C layer reads the value through the env capability,
derives from a copy, and wipes that copy. It is gated by `manifest.env` like
any other environment read, and mirrors the `"$VAR"` references
`databases.named` already accepts.

`passphrase = "..."` remains for callers that already hold the bytes, and says
in its own docstring that Hull cannot scrub it.

**The limit, stated rather than implied:** an env-carried secret still sits in
the process environment for the process's lifetime. That is the ordinary
trade-off and this does not remove it. What it buys is that the passphrase is
not ALSO in the script heap, where it would be unscrubbable, GC-visible and
reachable by any app code - and where a second key's passphrase, or a reused
one, would accumulate beside it.

### Why the error text is part of the contract

`aes256-ctr` is unauthenticated. A wrong passphrase does not fail in the
cipher; it decrypts to plausible garbage, and the only integrity signal the
format carries is `check1 == check2` inside the plaintext. So:

| case | what it says |
|---|---|
| wrong passphrase | "wrong passphrase", NOT "the file is corrupt" |
| no passphrase given | names `passphrase_env` and `passphrase`, since the next move is to add one |
| a cipher Hull does not read | names it, with the `ssh-keygen -p` that converts it |
| env var undeclared OR unset | ONE message for both, so a caller cannot probe the allowlist |

Nothing ever echoes the passphrase, its length, or a derived byte.

### What is trusted, and how it is checked

The KDF is vendored from OpenBSD rather than written (see
`vendor/bcrypt/README.md`): its entire value is agreeing byte-for-byte with
`ssh-keygen`, and a subtly wrong one is indistinguishable from a mistyped
passphrase. Both it and AES-256-CTR are pinned to published vectors - OpenBSD's
regress suite and NIST SP 800-38A F.5.5 - not to anything this repository
computed.

The end-to-end test uses **`ssh-keygen` as the oracle**: it writes an
encrypted key, Hull decrypts it, and the public key Hull derives is compared
against what `ssh-keygen -y` derives from the same file. A round-trip against
ourselves would pass just as happily with a wrong KDF, consistently wrong.

### Still not handled

`client_ed25519` (MariaDB-style), FIDO/`sk-` keys, and agent-held keys. An
**ssh-agent** client is the one worth naming: it would mean never handling a
passphrase OR a private key at all, which is strictly better than handling
either carefully. It needs a unix-socket / named-pipe capability Hull does not
have - `cap/net_stream.c` is TCP only - so it is a larger piece of work, not a
variation on this one.

## 12. Decisions taken

| question | decision |
|---|---|
| `hull/net` | **Not published.** The byte stream exists as PRIVATE native infrastructure (`cap/net_stream.c`) with one caller, the SSH stdlib. No `hull/net` module, no `net` manifest key. Applications never receive raw socket authority. Design: [`net_module_design.md`](net_module_design.md) section 2. |
| SSH protocol language | **Lua**, deliberately, not C. See section 11a. |
| AEAD | **`aes256-gcm@openssh.com`**. One consequence needs a follow-up call: mbedTLS lives in the composable TLS feature, not the base, so an SSH app with no HTTP links no AES-GCM. See `net_module_design.md` section 5. |
| Lua vs JS | **Lua only for v1**, JS to follow. Section 8 option 1. |
| `hsctl` language | **Lua.** |
| Key passphrases | **Supported, and NAMED rather than carried.** `aes256-ctr` + `bcrypt` only; `passphrase_env` passes the env var's NAME so the value never becomes a Lua string. See section 11c. |
| Tunnelled reach | **A SECOND grant, `ssh.tunnel`.** `connect` keeps meaning the SSH destination and the login; `tunnel` names the relay dialled to get there. Both are checked, both fail closed. See section 11b. |

Phases 2 to 5 (pure Lua) are unblocked once `hull/net` N1 lands.
