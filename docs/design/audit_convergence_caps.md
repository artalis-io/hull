# Design amendment: audit convergence §6.4 / §6.5 as implemented

- **Status:** Implemented (phase 5, part 1)
- **Amends:** `docs/design/audit_convergence.md` (on the `design/audit-convergence-phase1`
  branch) §6.4 "Concurrency and input caps" and §6.5 "Engine complexity tests"
- **Not covered here:** §6.3 (VM thread / preemption), a separate change

The main design is not on `main` yet, so this file records how §6.4 and §6.5 were built and
where the implementation departs from the design text.

## §6.4 Concurrency caps

**Where.** A pre-body Keel middleware registered first in serve.c (`hl_serve_conc_mw`), so a
refused request costs no body read and runs no script. The bookkeeping is a header-only limiter
(`include/hull/shared/req_limit.h`): a table with one slot per connection the server can hold,
keyed by the `KlHttpConn` pointer (a connection carries one request at a time; HTTP/2 is not
enabled in Hull). Admission walks the table once to count the client's and the route's entries.

**In flight.** From admission to Keel's access-log hook (the response is sent), or to the next
request on the same connection. Keel has no connection-close hook, so a connection that closes
while its response is still being written leaves its entry behind; when the cap would refuse,
the limiter first sweeps entries whose connection Keel has released (its response header buffer
is freed on release) and counts again. A handler whose client hung up keeps its entry until it
finishes: it is still using the VM.

**Defaults and refusals.** Half the connection limit per client (`hl_req_per_ip_default`:
`--max-connections` / `HL_REQ_PER_IP_DEFAULT_DIVISOR`, 128 at the default 256), `429` with
`Retry-After: 1`; a route at its `route_concurrency` limit answers `503` with `Retry-After: 1`
(the route is out of capacity, not the client at fault). WebSocket upgrades to a WebSocket route
are exempt; SSE streams count while open.

*Why half, not 8.* The first cut defaulted to 8. That refused ordinary single-address traffic: a
browser opens six HTTP/1.1 connections per host and each open SSE stream holds one, an office
NAT or a CGNAT address carries many users, a load test from one host runs 100 connections, and
`tests/e2e_refusal_codes.sh` (65 requests from localhost to reach the 64-fetch cap) failed with
429s. The cap's purpose is to bound one client's share of the server, so it is sized against the
server: with half the slots per client, one client cannot starve the others, and it scales with
`--max-connections`. An app with slow handlers and many clients lowers it in the manifest.

**Configuration.** Manifest `limits.per_ip_concurrency` (`0` = off), `limits.route_concurrency`
(pattern -> limit; the app's route pattern, or a `/x/*` prefix), `limits.trusted_proxies`; the
operator flag `--max-requests-per-ip N` overrides the manifest (a DOWNGRADE row in
`hl_runtime_flags()`, so `--hull-max-requests-per-ip` in a built binary).

**Client key.** The TCP peer, IPv4 as itself and IPv6 by its /64 (the stdlib's per-client key).
For a peer in `trusted_proxies` (host-match syntax: addresses and CIDRs), the last entry of the
last `X-Forwarded-For` header - the rule `hull.web._request.client_ip(req, true)` uses. One
difference: the C side takes that entry only when it is an IP address and otherwise keys on the
proxy, where the stdlib returns whatever text is there; a non-address cannot be a client key.

**Behind a reverse proxy** every request shares the proxy's address, so the default cap would
apply to all users together. The documented remedies: list the proxy in `trusted_proxies`, use
Keel's PROXY protocol, or raise / disable the cap and rely on the proxy's limits.

## §6.4 Input caps

The design says "regex source length when the pattern comes from a request" and "JSON ... for
request bodies". Neither engine can tell where a string came from, and Hull has no `req.json()`
(apps call `json.decode(req.body)` / `JSON.parse(req.body)`), so the caps are global ceilings on
the app VM, with defaults far above real payloads (`include/hull/limits/input.h`):

| Cap | Default | Where |
|---|---|---|
| JSON nesting depth | 128 | Lua `hull.json` (via the stdlib-only `hull._limits`), JS `JSON.parse` / `JS_ParseJSON` (QuickJS patch 0012) |
| JSON keys per object | 100000 | same |
| RegExp source | 64 KiB | QuickJS `js_compile_regexp` (patch 0012) |
| normalize / localeCompare input | 1 MiB | QuickJS `js_string_normalize1` (patch 0012) |

The key count is per object rather than per document: audit 40 treats a 200000-record array as
ordinary, and the cost the cap bounds (one object's property table) is per object; a document's
total size is already bounded by the body limit and the heap. Request bodies keep their existing
cap (`--body-max-size`, default 1 MiB). Patch 0010 already made normalize, localeCompare and
RegExp compilation linear; these ceilings are defence in depth. All four are configurable in the
manifest's `limits`.

## §6.5 Engine complexity tests

`tests/complexity/complexity.c` (`make complexity`, CI job `engine-complexity`) runs the raw
vendored engines - Hull's patches, no Hull runtime - so the numbers are the engine's own. Each
case defines `setup(n)` (untimed) and `work(state)` (timed); the time is the thread CPU time
(`CLOCK_THREAD_CPUTIME_ID`, so time preempted by other load is not counted), the least of five
runs (noise only adds time) at n and at 2n, the assertion t(2n)/t(n) < 2.5, and a failing case is
measured again, up to three attempts, before it counts. The verdict is a ratio, so a slower
machine or a sanitizer build needs no adjustment. The suite runs in about 30 s and prints every ratio.

Inputs are ordinary worst-case shapes: long runs of combining marks in mixed classes, large
character classes, many alternatives, long lookbehinds, deep and wide JSON, objects with many
keys. Hash structures run on the keyed hash; no input is built to collide. The one collision case
uses QuickJS's test-only hook (`JS_TestForceAtomHash`, `QJS_HULL_TEST_HOOKS`) to put every atom
on one chain; insertion is then quadratic by construction, and the case asserts instead that the
charged work grows in step with the time (time ratio / charge ratio within 2.5x), which is what
lets the budget stop it in Hull. Map / Set have no such hook and are tested for linearity only.

The Lua weak-table ephemeron case is deferred until the ephemeron fix (PR #969) is on main.

Measured on an Apple M-series laptop (one run): every ratio between 1.84 and 2.47; the forced
collision case at 3.6-3.9 in time with the charge tracking it (time / charge 1.03-1.09).
