# The private outbound byte stream (cap/net_stream)

Design record. **Superseded in one important respect: there is no public
`hull/net` module and no `net` manifest capability.** The transport described
here is PRIVATE native infrastructure with exactly one caller, the SSH stdlib.
Sections 1 to 5 describe it accurately; section 4's capability shape moved to
`ssh.connect` (see `ssh_module_design.md`).

**Objective.** A small outbound TCP byte stream, reachable only from trusted
stdlib code, so a protocol implemented in a safe language can use the network
without the application gaining raw socket authority.

Prior art and the closest thing to a specification already in the repo:
[`smtp_keel_client_design.md`](smtp_keel_client_design.md) section 4.
First consumer: [`ssh_module_design.md`](ssh_module_design.md).

## 1. Why this exists

Hull can serve the network but cannot dial it from stdlib code.

(An earlier draft of this section said three protocol clients privately
re-implement the connect sequence. That was wrong, and section 6 is corrected
with it. `cap/http.c` uses Keel's own `KlHttpClient`, pooling included, and the
WebSocket client uses `kl_ws_client_connect`; neither owns a transport. The
private ones are SMTP and the SQL wire clients, and both are worker-model by
design.)

The composition that is missing from the public surface is:

```
kl_connect_op_init / _start / _cancel        async connect, resolve,
kl_connect_op_on_resolved / _on_attempt_*    IPv4+IPv6 racing, delay, deadline
KlStream                                     ordered bounded writes, read
                                             delivery, backpressure, cancel
```

`src/hull/cap/smtp_transport.c` is the most complete composition of it.
Nothing exposes any of it to scripts, so a pure-Lua protocol implementation is
impossible today regardless of how simple the protocol is.

`hull/net` makes that composition public, once, under a capability.

## 2. Why it is NOT a public module

An earlier draft of this section argued the opposite, on a correct observation
and a wrong conclusion.

The observation stands: Hull's require gate is `if (spec)`, so it only fires
for modules in the registry. A `_`-prefixed "private" Lua module would be
UNGATED, and publishing `hull/net` as a registry module is the only way to make
it refusable.

The conclusion was wrong because it assumed the stream had to be reachable from
application Lua at all. It does not. The layering is three, not two:

```
Application            require("hull.ssh"); ssh.connect{...}
                       authority: ssh = { connect = {...} }
      |
      v
Trusted stdlib         the SSH protocol and state machine, in Lua
      |
      v
Private native         cap/net_stream.c: connect / read / write / close
                       cap/net_policy.c: is this destination allowed
```

The application never receives the stream. It asks the SSH module for an SSH
connection, and the SSH module obtains a stream only after the policy check
passes. So there is no ungated-module problem to solve: there is no module.

What this buys, beyond one fewer public API:

- **The authority an app declares matches what it actually needs.**
  `ssh = { connect = { hosts = {"*.local"}, users = {"operator"} } }` tells a
  reviewer what the program may do. `net.connect = *.local:22` tells them a
  socket may be opened and leaves the rest to trust.
- **The app cannot be talked into speaking something else** over that socket,
  because it never holds it.
- **Protocol gaps stay visible as stdlib gaps.** With a public byte stream, the
  answer to "Hull does not support protocol X" becomes "write it yourself in
  Lua", which mostly produces half-correct protocol code in applications.
  Without one, the gap gets filed.

If a second genuine consumer appears, promoting this to a public module is a
small change, and by then its requirements will be known. The reverse -
un-shipping a capability - is not small.

## 3. Public API

Lua first (v1). JS follows with the same shape in camelCase.

```lua
local net = require("hull.net")

-- Capability is checked BEFORE resolution and before any socket exists.
local s, err = net.connect({
    host       = "spark-7468.local",
    port       = 22,
    timeout_ms = 10000,           -- whole-connect deadline
})
if not s then
    -- err.code: capability_denied | resolve_failed | connect_failed | timeout
end

s:write(bytes)                                  -- bounded, all-or-none admission
local chunk = s:read(4096, { timeout_ms = 5000 })  -- nil on clean EOF
s:close()                                       -- graceful (FIN, drain)
s:cancel()                                      -- abortive, immediate teardown
s:deadline(ms)                                  -- rearm the operation deadline
```

Properties:

- **Async by the existing model.** A cap creates an `HlAsyncCtx` and the binding
  calls `lua_yieldk`, exactly as `mod_http_client.c` does today. No busy-wait,
  no second event loop, no new scheduling concept.
- **Byte-safe.** Lua strings are byte-clean; JS gets `ArrayBuffer`. Reads return
  what arrived, not "one protocol record": callers frame their own protocol.
- **Backpressure is visible.** `write` admits all-or-none against a bounded
  queue and yields until drained, rather than growing an unbounded buffer.
- **Cancellation is a stream property**, so every consumer inherits it. This is
  what lets `hull/ssh` tear down a connection mid-handshake cleanly.
- **No TLS in v1.** SSH does its own crypto. A later `s:start_tls(opts)` is the
  natural home for what `cap/smtp_tls.c` does privately today, and section 6
  treats that as the SMTP migration step, not a v1 obligation.

## 4. Capability

Follows `databases.dynamic` and `kv.dynamic` exactly, including the shared
`hl_host_match_any_env` matcher (exact host, `*`, `*.suffix`, CIDR for IP
literals, `$VAR` env refs) and fail-closed semantics.

```lua
app.manifest({
    modules = { "hull/net@1" },
    net = {
        connect = {
            hosts = { "*.local", "10.0.0.0/8" },
            ports = { 22, 443 },        -- required; no implicit default
        },
    },
})
```

- No `net` block, or an empty one, denies everything.
- `ports` is **required** here, unlike `hull/ssh` where it defaults to `{22}`.
  A general TCP capability should not have a convenient default port.
- The check runs before DNS. Denial is `capability_denied`, distinct from a
  timeout or connect failure.

### 4.1 Narrowing, not re-exporting

`hull/ssh` is built on `hull/net` but must **not** grant it. An app declaring
`hull/ssh@1` gets SSH to its allowlisted hosts on port 22, and no raw TCP at
all.

Mechanically: `hull/ssh` declares its own `ssh.connect` capability, and the SSH
module's internal use of the stream is authorized against the SSH policy, not
against `net.connect`. The module registry's dependency edge (`hull/ssh` needs
`hull/net`) admits the *module*, never the *authority*. This mirrors how
declaring `hull/web/middleware/session` auto-admits `hull/db` without granting
arbitrary DSNs.

**This needs an explicit test:** an app with only `hull/ssh@1` must fail closed
on `require("hull.net")` followed by `net.connect`.

## 5. Where AES-256-GCM lives (open decision)

You chose `aes256-gcm@openssh.com` for SSH. One consequence needs a decision,
because it is not free.

mbedTLS is **not in the base**. Since the composable-base work it lives in
`libhull_feature-tls.a`, composed only when `needs_tls` is set (an HTTP module,
or `--with=postgres`/`mysql`). An SSH app with no HTTP would therefore link no
mbedTLS and have no AES-GCM.

| option | cost | risk |
|---|---|---|
| **A. `hull/ssh` declares `needs_tls`** | pulls the whole TLS feature (mbedTLS) for one AEAD, in SSH apps only | low: reuses a vetted implementation |
| B. self-contained AES-GCM in the base crypto cap | no TLS dependency; precedent exists (base SHA-256 is self-contained for pure-compute) | **higher**: a hand-rolled AES plus GHASH in a security-critical path |
| C. AEAD as its own composable feature | clean layering | over-engineered for one algorithm |

**Recommendation: A.** The size cost falls only on apps that use SSH, and it
reuses an implementation that is already audited and already shipping. B trades
a binary-size win for writing new cryptographic code, which is the wrong trade
given the stated priority order (correctness, then security, then size).

Worth noting plainly: SSH does not use TLS, so option A links a TLS stack for a
cipher. That is a wart. It is a smaller wart than a bespoke AES-GCM.

## 6. Migration path: mostly there is not one

Corrected. This section previously proposed migrating SMTP, then WebSocket,
then maybe HTTP onto hull/net. Two of those three were never duplicating it.

| client | what it actually uses | port to hull/net? |
|---|---|---|
| **HTTP** | Keel's `KlHttpClient`, including `kl_http_client_request_pooled` | **No.** It is not duplicating this layer; it is using a higher-level Keel API. Porting would mean giving up connection pooling, HTTP/2, redirects, streaming bodies and the TLS integration, and then reimplementing them. |
| **WebSocket** | Keel's `kl_ws_client_connect` | **No.** Same reason: Keel implements the protocol, so Hull delegates. |
| **SMTP** | its own `cap/smtp_transport.c`, on a pool worker | **Not worth it.** A genuine private transport, but worker-model by design. Porting is a re-architecture of shipped, working code for tidiness, not a swap. |
| **SQL wire (pg / mysql)** | its own `cap/db_transport.c`, blocking on a worker | **No.** Explicitly a different execution model, and itself already the product of consolidating two byte-identical copies. |

So the rule that falls out, and the one worth keeping:

> **hull/net exists for protocols Keel does not implement.** Where Keel ships a
> client, Hull uses it.

SSH is the case that justifies hull/net: Keel has no SSH client, the protocol
is small and self-contained, and it had to exist at all. That is a narrow
charter on purpose. A future protocol with no Keel implementation (a custom
wire format, a line protocol, an agent transport) is the next legitimate
consumer; HTTP is not.

## 6a. Scheduling model (corrected during N1b)

The first draft of this document said hull/net would follow "the SMTP model".
Reading the two existing transports properly shows that is wrong, and the
correction matters enough to record rather than quietly fix.

**Both existing transports pin a thread for the life of the operation.**

| transport | event context | runs on |
|---|---|---|
| `HlDbTransport` | private, pumped only during connect, then `set_blocking()` | the calling worker thread |
| `HlSmtpTransport` | private (`t->ev`), pumped for the whole conversation | a pool worker (`smtp_worker.c`) |

That is fine for a request-scoped operation. It is not fine for SSH:

- The default pool is **4 workers** (`serve.c`, `--workers N`).
- SMTP caps concurrent sends at `max(1, floor(W / 2))` = **2** by default
  (`cap/smtp_admit.c`), deliberately leaving workers for db and compute.
- An SSH connection is **long-lived**, not request-scoped. A worker would be
  held while idle, waiting on remote output.

So a worker-per-connection hull/net could not deliver hsctl's eight concurrent
node connections, and raising `--workers` would be the wrong fix: it couples
connection concurrency to a knob meant for CPU work, and pins threads that are
doing nothing. The point of an event loop is not to need a thread per idle
connection.

**hull/net is therefore event-loop integrated**, which is new for a Hull
byte-stream but not new for Hull. Every piece has a precedent:

| piece | precedent |
|---|---|
| long-lived client connection on the MAIN event context | `kl_ws_client_connect(KlEventCtx *ev, ...)`, already used by `ws.connect`, whose callbacks fire on the event-loop thread |
| `KlConnectOp` driven to a winning descriptor | `cap/smtp_transport.c`. Note it needs NO event context: KlConnectOp and KlStream are hook-driven state machines, so Keel's connect logic sits on Hull's scheduler and the transport stays backend-agnostic |
| pull-style reads over push-style delivery | the multipart body reader: `hl_cap_multipart_park` parks the coroutine on NEED_DATA and the `on_data` callback resumes it |
| bounded writes with backpressure | `KlStream` (`kl_stream_write` / `_flush` / `_on_write_complete`) |

`KlStream` is transport-agnostic: it takes writer/submit callbacks and read
arm/disarm/deliver callbacks rather than owning a descriptor, so wiring it to
the main event context is a composition question, not a Keel limitation.

Consequence for the API in section 3: unchanged. `s:read` and `s:write` look
the same to Lua either way, which is why choosing the loop-integrated model now
does not foreclose anything, and why getting it wrong would have been expensive
to undo later.

## 7. Feature composition

`hull/net` needs Keel, which since the composable-base work is itself a composed
feature (`libhull_feature-keel.a`, composed when `needs_http`). A compute-only
app links no Keel at all.

So `hull/net` introduces a second trigger for the Keel feature:
`needs_net = needs_http or declares("hull/net") or declares("hull/ssh")`.

This is a small change to `feature_compose.lua` and the `needs_*` gates, and it
keeps a pure-compute app free of Keel exactly as today.

## 8. Security invariants to test

- Capability is checked before resolution; no DNS query escapes for a denied host.
- `hull/ssh` alone does not grant `net.connect` (section 4.1).
- Denial is deterministic and distinct from timeout or connect failure.
- A closed or cancelled stream fails closed on every subsequent operation.
- Cancellation detaches the connect op, all racing descriptors and all timers
  before returning ownership (the `smtp_keel_client_design.md` section 4.4
  contract).
- Write admission is bounded; a slow peer cannot grow an unbounded queue.
- Read returns only what arrived; no silent coalescing assumption.
- No ambient connect: an app without a `net` block cannot reach anything.

## 9. Phasing

| phase | content |
|---|---|
| N1 | `hull/net` cap: connect, read, write, close, cancel, deadline. Lua binding. Capability + gate. Unit and capability-denial tests. |
| N2 | JS binding at parity. |
| N3 | `s:start_tls()`; migrate SMTP onto `hull/net`, retiring `cap/smtp_transport.c` as a private duplicate. |
| N4 | WebSocket migration, if N3 shows the seam holds. |

SSH phase 1 depends only on N1.

## 10. Open items

1. **AES-GCM placement** (section 5). Recommendation A, needs your call.
2. **JS timing.** v1 is Lua-only by decision; N2 can land before or after SSH
   phase 2 depending on whether `hsctl` stays Lua.
3. Whether `hull/net` should expose `listen`/accept eventually. **Not in v1**,
   and deliberately: inbound is `hull/http-server`'s domain, and a general
   listen capability is a much larger authority question.
