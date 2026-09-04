# DNS resolver unification onto Keel - design record

The Keel v3 client-transport migration routed every first-party client and
server socket lifecycle, byte I/O, and event axis onto Keel: the HTTP server,
the HTTP client, the WebSocket client and server, SMTP
([`smtp_keel_client_design.md`](smtp_keel_client_design.md)), the PostgreSQL /
MySQL / Valkey wire clients on the shared `HlDbTransport`
([`pg_keel_transport_slice3.md`](pg_keel_transport_slice3.md),
[`mysql_keel_transport_slice4.md`](mysql_keel_transport_slice4.md),
[`db_transport_extraction.md`](db_transport_extraction.md),
[`valkey_keel_transport_slice5.md`](valkey_keel_transport_slice5.md)), and the
`hull agent request` / `status` tooling client (PR #457).

After that work, a whole-tree sweep finds **zero** raw `socket` / `connect` /
`send` / `recv` / `listen` / `accept` in first-party production code. The one
remaining raw network primitive is **blocking `getaddrinfo` name resolution**,
in exactly two places. Both slice records above deliberately deferred it (the PG
record: "no async DNS ... hostnames via a blocking `getaddrinfo` adapter"). This
record scopes whether and how to move that last primitive onto Keel's resolver
seam. It is design-only: no code, no behavior change. The resting recommendation
is to leave it (Option C); the preferred triggered follow-up (Option B) carries
ownership, capacity, and shutdown contracts that must be treated as first-class
design work.

## 1. Current state: what "on Keel except DNS" means today

Two sites do their own blocking `getaddrinfo`, then hand the resulting
`KlSockAddr` list to Keel's connect:

- `cap/db_transport.c` `resolve_addrs()` (used by PostgreSQL / MySQL / Valkey).
- `cap/smtp_transport.c` (the SMTP transport).

Both share the same shape:

- An **IP-literal fast path** (`kl_sockaddr_parse`, numeric-only, no DNS) that
  already needs no resolver. This matches the IP-literal-only
  `databases.dynamic` CIDR gate, so a sandboxed DB app resolves nothing.
- A **hostname path**: `getaddrinfo(AF_UNSPEC, SOCK_STREAM)`, mapping each A /
  AAAA result to a `KlSockAddr` (IPv6 scope preserved), capped at
  `KL_CONNECT_MAX_ADDRS`.
- The result feeds `KlConnectOp` (`kl_connect_op_on_resolved` /
  `_on_resolve_failed`), which races the connect (Happy Eyeballs) over Keel's
  socket provider on a private `KlEventCtx`.

Resolution completes **synchronously, inline, before** `kl_connect_op_start`:
`co_start_resolve()` today only yields the pre-filled list. The connect-op is
already async-shaped (its resolve hook may complete later), so the seam for
async resolution exists but is unused.

Properties the blocking `getaddrinfo` path has today, that any replacement must
be measured against:

- **Full OS resolution via glibc NSS / nsswitch**: `/etc/hosts`, mDNS (`.local`
  / Bonjour), LDAP / sssd / NIS, systemd-resolved split-DNS and per-link
  resolvers, `/etc/resolv.conf` nameservers + `search` + `ndots`. On macOS, the
  system resolver path.
- **AF_UNSPEC dual-stack** (A + AAAA) with IPv6 scope IDs.

**DNS is deliberately outside the timed budgets - a frozen contract, not a
wart.** Both transports arm their connect / operation deadline so that DNS does
not count against it:

- `db_transport.c`'s D3 deadline is scoped to **"TCP establishment only"**;
  resolution runs before it.
- `smtp_transport.c`'s **post-resolution operation deadline (Dop)** is an
  absolute instant **set once right after resolution**; every post-resolution
  stage bounds by `min(Dop, now + stage_budget)`.
- SMTP's cancel poll is wired only after DNS, so `getaddrinfo` is the
  **documented non-interruptible exception** (it runs before cancellation is
  observable).

So the two costs of the current path are, precisely: it **blocks the
event-loop thread** for the duration of resolution, and a hung resolver **is
not bounded by the connect / operation deadline or a cancel** (by the frozen
design, not by accident). Any async move must therefore make a conscious
decision about DNS's relationship to those budgets (section 3).

Hull-wide precedent: the outbound HTTP client already accepts blocking DNS.
`cap/http_async.c` sets Keel's `system_dns = 1`, forcing Keel's client to
resolve through blocking `getaddrinfo` (to preserve `/etc/hosts` + search
domains) rather than its built-in async resolver. "Blocking `getaddrinfo`, for
NSS fidelity" is already a deliberate, consistent Hull posture, not a
db/smtp-only wart.

## 2. What Keel offers

- **`KlResolver` vtable** (`keel/resolver.h`): a pluggable async DNS interface
  (`resolve` / `cancel` / `destroy`). `resolve()` must not block; it calls a
  completion callback on the event-loop thread, and may complete synchronously.
  It drops into `KlHttpClientConfig.resolver`, or can be driven directly against
  a standalone `KlEventCtx` (which is what the db/smtp transports own).
- **`kl_dns_resolver_create`** (`keel/dns_resolver.h`): Keel's built-in async
  resolver over non-blocking UDP. It is a **capable** resolver, not a toy: it
  runs the **A and AAAA queries concurrently**, collects **multiple addresses**
  (up to `KL_RESOLVE_MAX_ADDRS`, preferred first), retransmits on timeout, and
  implements **RFC 7766 TCP fallback** for truncated responses. It reads
  `resolv.conf` (nameservers / search / ndots) and a hosts file via config
  (`resolv_conf_path`, `hosts_path`). The **freestanding build** is the
  UDP-only case (no TCP fallback). The one thing it is not is the glibc nsswitch
  chain (section 4).
- **`kl_resolver_cache_create`** (`keel/resolver_cache.h`): a caching decorator
  around any `KlResolver` (subject to the lifetime caveat in section 6).
- **Ownership:** `kl_dns_resolver_create` **borrows** its `KlEventCtx`; the
  resolver must be destroyed **before** that context.

## 3. The integration seam and the deadline-contract decision

Because `KlConnectOp` already models resolution as a hook that may complete
asynchronously, wiring a `KlResolver` is mechanically local to the two
transports: `co_start_resolve()` calls
`resolver->resolve(resolver, &t->ev, host, port, on_resolve_done, t)`;
`on_resolve_done()` maps `KlResolveResult.addrs` into `t->addrs` and calls
`kl_connect_op_on_resolved()` / `_on_resolve_failed()`; the IP-literal fast path
stays inline.

**This is not behavior-neutral.** `KlConnectOp` arms its deadline *before*
`start_resolve()`. Today resolution runs synchronously before the op and is
deliberately excluded from the timed budgets (section 1). Folding resolution
into the op makes DNS consume:

- `db_transport.c`'s D3 deadline, frozen as **"TCP establishment only"**, and
- SMTP's **Dop**, frozen as a post-resolution operation deadline set once after
  resolution.

The record must therefore choose one of:

- **(i) Fold DNS into the op budget.** Simplest wiring, but DNS now counts
  against the connect / operation deadline; both frozen contracts change, and a
  slow resolver can exhaust the connect budget before a single SYN. This is an
  unadvertised contract change and is **rejected** here.
- **(ii) Separate DNS and post-resolution deadlines.** A distinct
  DNS-resolution deadline precedes the existing connect / operation deadline, so
  D3 and Dop stay exactly as frozen. This is the contract-preserving path and
  the one any async adoption should take. It is more machinery - a second timer
  with its own cancel / teardown ordering - not a free seam.

**Test seams are preservable**: `db_transport_test_resolve`
(`HL_DB_TRANSPORT_TEST_HOOKS`) and the `HL_SMTP_TEST_HOOKS` resolve override
become "inject a fake `KlResolver`" (or the address-injection seam stays ahead
of the resolver call), keeping the deterministic-address transport tests intact.

## 4. The central tension: NSS / nsswitch semantics

Keel's hosted resolver is capable in every dimension that matters for
robustness (concurrent dual-stack, multiple addresses, RFC 7766 TCP fallback,
`resolv.conf` + hosts via config). The single semantic gap versus `getaddrinfo`
is that it is a **DNS client, not the glibc NSS / nsswitch chain**: no mDNS
(`.local` / Bonjour), no LDAP / sssd / NIS, no systemd-resolved split-DNS or
per-link resolvers, no NSS plugins. (The freestanding build additionally drops
TCP fallback.)

Impact by deployment:

- **Public FQDN or IP-literal endpoints** (the common DB / SMTP case): fully
  covered. No regression.
- **Containers / Kubernetes**: usually `/etc/resolv.conf` pointing at a cluster
  resolver - usually fine, with ndots / search-domain / headless-service edge
  cases to watch.
- **Enterprise / corporate**: LDAP / sssd / AD-integrated resolution - **not
  covered**; a real regression.
- **macOS**: `.local` / Bonjour - **not covered**.
- **Split-horizon VPN**: systemd-resolved per-link resolvers - **not covered**.

Hull cannot assume the deployment, so a blanket swap trades a correctness
guarantee (whatever the OS resolves, Hull resolves) for a benefit (non-blocking,
deadline-bounded DNS) that only matters when a resolver hangs.

## 5. Sandbox: no new authority under today's profiles

An earlier draft of this record claimed Option A adds an outbound
UDP-to-nameserver rule and could shrink the NSS surface. Both are wrong under
the current profiles:

- Hull's `network_outbound` grant **already permits outbound DNS/UDP** to the
  resolver. On Linux the pledge set adds `dns netlink unix` (`dns` = UDP/IP
  queries to the resolver, `netlink` = interface enumeration, `unix` = nsswitch
  IPC); on macOS it adds `dns`. So a UDP resolver exercises authority Hull
  **already grants** - Option A adds **no clearly new authority**; it changes
  *which* existing authority is exercised.
- The NSS surface (`netlink` / `unix`) cannot be removed by this change, because
  the HTTP client keeps `system_dns = 1` (blocking `getaddrinfo`, which needs
  NSS). Removing that surface would require **every** resolution path -
  including HTTP - to leave `getaddrinfo`. Feature-specific narrowing is a
  separate analysis, not a benefit of moving DB/SMTP alone.

## 6. Options

- **Option A - adopt `kl_dns_resolver` (hosted UDP async, RFC 7766 TCP
  fallback).** Benefit: non-blocking resolution, and - with the section 3 (ii)
  design - deadline-bounded, cancellable DNS. Cost: the NSS / nsswitch-parity
  loss (section 4); no new sandbox authority but a different resolution engine
  (section 5); and the ownership / lifetime constraints (section 2, plus the
  caching caveat below).
- **Option B - a Hull `KlResolver` that wraps `getaddrinfo` off-thread.** Keep
  OS / NSS semantics and the current sandbox posture; run the blocking
  `getaddrinfo` on a worker (or a dedicated resolver thread) and complete on the
  `KlEventCtx` via an eventfd / self-pipe wakeup. It is
  **resolution-semantics-preserving, not risk-free**:
  - it bounds the **caller's wait**, not the underlying `getaddrinfo` work;
  - **cancellation** requires retained per-request state and late-completion
    suppression (a result that arrives after the connect was cancelled must be
    dropped, and its buffers reclaimed, without touching a freed op);
  - a **shared worker pool**'s resolution jobs consume pool capacity and can
    delay shutdown; **detached dedicated threads** risk unbounded accumulation
    under churn. The lifecycle needs an explicit bound.
  Keel ships no such resolver, so Hull writes the vtable plus the wakeup and
  cancellation plumbing.
- **Option C - status quo (blocking `getaddrinfo`).** NSS-correct,
  sandbox-tested, and already the Hull-wide posture (http `system_dns`). The
  only wart is the deliberate non-interruptible exception: a hung resolver is
  not bounded by the connect / operation deadline or a cancel.
- **Option D - hybrid.** Option A behind an explicit opt-in (a DSN parameter,
  env, or manifest flag), Option C as default. The sandbox is unchanged
  (section 5), but two resolution engines must be maintained and tested, and the
  section 3 (ii) deadline design still applies to the opt-in path.
- **Caching caveat (Options A and D).** `kl_resolver_cache_create` caches only
  for the life of its resolver, which borrows a `KlEventCtx`. DB transports
  **free their private `KlEventCtx` after each connection**, so a per-transport
  resolver + cache dies with the connection and has essentially **no
  cross-connection value**. Useful caching needs a longer-lived owner - a
  process- or pool-scoped resolver and event context - which is its own design
  effort, not a free decorator.

## 7. Recommendation

- **Resting state: Option C.** NSS-faithful, sandbox-tested, and already
  Hull-wide; the two `getaddrinfo` sites are a deliberate, contract-defined
  exception (DNS excluded from D3 / Dop), not a raw-socket leftover. Close this
  thread unless a concrete trigger appears (a production hang traced to blocking
  DNS, or a hard requirement for deadline-bounded resolution).
- **Preferred triggered follow-up: Option B**, provided its **ownership,
  capacity, and shutdown contracts are treated as first-class design work** -
  retained request state and late-completion suppression, worker-pool capacity
  versus shutdown drain, or a bounded dedicated-thread lifecycle - together with
  the section 3 (ii) separate-DNS-deadline design so D3 and Dop stay frozen. It
  preserves OS resolution semantics and the sandbox posture while removing the
  loop-thread block and making the DNS wait deadline-bounded.
- **Do not default to Option A.** The nsswitch-parity loss is a correctness
  regression for enterprise, mDNS, and split-DNS deployments Hull cannot rule
  out, and it buys non-blocking DNS whose only payoff is a hung resolver -
  which Option B delivers without the semantic change. Reserve Option A (or the
  Option D opt-in) for a deployment set known to be plain-recursive, and only
  after the resolver-ownership / caching-lifetime design (section 2, section 6)
  is settled.
- **First step if pursued:** a spike of Option B behind the existing
  `db_transport_test_resolve` seam, exercised against the connect-deadline and
  cancel tests on all three sandbox backends (Linux pledge, macOS Seatbelt,
  cosmo), with the resolver-ownership and separate-DNS-deadline designs settled
  up front rather than discovered mid-implementation.

## 8. Non-goals and scope

- This is **not** a raw-socket cleanup: the `socket` / `connect` / `send` /
  `recv` / event-axis migration is complete across all first-party clients and
  servers.
- No behavior change is proposed or shipped by this record.
- It reuses the existing `KlConnectOp` resolve seam; it introduces no new
  transport abstraction. It does, however, require an explicit deadline-model
  decision (section 3) and, for any caching, a longer-lived resolver-ownership
  design (section 6) - neither is a free seam.
- `inet_pton` in `utils/host_match.c` (CIDR / IP-literal allowlist matching, not
  I/O) and the `setsockopt(SO_RCVTIMEO/SNDTIMEO)` io-timeout in `db_transport.c`
  (a deliberate native-fd reach-through, no Keel provider op) are out of scope:
  neither is name resolution or socket I/O.
