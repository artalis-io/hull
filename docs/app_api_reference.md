# Hull app-facing API reference

Moved out of `CLAUDE.md` (which is the contributor guide and has a size
budget). This is the reference for the stdlib, middleware, and the
WebSocket / SSE / multipart / static / SMTP / timer / WASM compute / GPU /
buffer / TUI surfaces that apps call. Lua and JS are functionally
equivalent; JS uses camelCase.

## Database connections (`hull/db`)

Handles-only API: there is no top-level `db.*`; every query goes through a
connection object. Semantics (per-connection `async`/`udf`, the registry, the
`db.async` + `:memory:` caveat, dynamic-connection policy) are in `CLAUDE.md`
("PostgreSQL + multi-backend DB").

```lua
local db = require("hull.db").default()   -- the default connection (-d / "default")
db.query("SELECT ...", { ... })
db.exec("INSERT ...", { ... })
local cache = require("hull.db").connect("cache")  -- a named connection
cache.query(...)
local tmp = require("hull.db").open(dsn)   -- a caller-owned dynamic connection
tmp.query(...); tmp.close()                -- app owns it: close() or let GC finalize
```

```javascript
import { db as dbModule } from "hull:db";
const db = dbModule.default();            // JS: acquire the default connection
db.query("SELECT ...", [ ... ]);
const cache = dbModule.connect("cache");
const tmp = dbModule.open(dsn);           // caller-owned dynamic connection
tmp.query(...); tmp.close();
```

**Transactions never span a wait.** The default, named and internal
connections are each ONE connection shared by every request, SSE event and
timer. While a handler waits (`http.fetch`, `db.async`, `hull.sleep`,
`compute.async`, `worker.dispatch`, `smtp.send`, a multipart body read, a
task's `wait`) other handlers run on the same connection, so a transaction
cannot stay open across the wait: the wait raises `... cannot wait while a
transaction is open on database connection 'NAME'`. Commit or roll back
first, or do the waiting outside. A `db.open` handle counts too (`'db.open'`):
an app may keep one at module level and use it from every request, so it is
shared just the same. For the same reason `db.batch(fn)` takes a synchronous
`fn`: in JS an `async` function is refused with a `TypeError` before the batch
begins, and a function that returns a Promise / thenable is refused with the
batch rolled back (it used to commit at the first `await`, running the rest in
autocommit). A transaction an entry leaves open - a request, middleware, SSE
event, timer or WebSocket callback that returned, raised or parked - is rolled
back right then, and again before any entry starts or a parked handler
resumes, so no other code ever runs inside it.

**`db.async` runs one statement on a pooled worker connection**, which the next
`db.async` op on that thread reuses. A transaction cannot span ops there: an op
that leaves its worker connection inside a transaction (`BEGIN`, `START
TRANSACTION`, a multi-statement string that opens one) has it rolled back and
fails with `a transaction cannot span db.async operations ...`. Use `db.batch`
on the connection for a transaction.

Named and dynamic connections are declared in the manifest. A DSN of exactly
`"$VAR"` / `"${VAR}"` is an env reference resolved at open time.

**Every referenced variable must be declared**, in `secrets` or in `env`, or the
app fails to load with a message naming the field and the variable. List it in
`secrets` when only the manifest should read it: a secret reaches the connection
or allowlist that names it and is never readable through `env.get`. List it in
`env` only if scripts also need it. The same rule covers every field that
accepts a reference: `hosts`, `databases.named`, `databases.internal`, `databases.dynamic.hosts`,
`kv.dynamic.hosts`, `ssh.connect.hosts` and `ssh.tunnel.hosts`. (Without it, a
reference could read any variable in the environment, an SQLite file name, for
instance, can be read back with `PRAGMA database_list`.)

```lua
app.manifest({
    modules = { "hull/db@1" },
    secrets = { "DATABASE_URL" },          -- readable by "$DATABASE_URL" only
    databases = {
        named = {
            cache   = "./cache.db",       -- SQLite file (literal)
            primary = "$DATABASE_URL",    -- postgres:// from $DATABASE_URL (env ref)
        },
        dynamic = { hosts = { "*.rds.amazonaws.com" }, schemes = { "postgres" } },
        internal = "$HULL_INTERNAL_URL",  -- the stdlib's own _hull_* tables (optional)
    },
})
```

**MySQL / MariaDB: `_hull_*` collation.** Hull creates its own tables with
`COLLATE utf8mb4_bin` so role names, inbox ids and dedup keys compare
exactly (the server default `utf8mb4_0900_ai_ci` makes `'Admin' = 'admin'`).
Tables created by an older Hull keep the case- and accent-insensitive
collation - nothing converts them and nothing warns. Convert them once with
`ALTER TABLE <t> CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_bin` for
each `_hull_*` table (with `FOREIGN_KEY_CHECKS = 0` around it); CLAUDE.md
("MySQL/MariaDB specifics") has the generator query.

**`databases.internal`** (optional). Where the stdlib keeps its own `_hull_*`
tables: sessions, auth-flows tokens and lockouts, TOTP, the audit log, RBAC,
idempotency keys and attachment metadata. Point it at a database (or a
schema) reached under a role your app's connection has **no grants on**, and
the database itself keeps app SQL away from those tables. Without it they
share the app's default connection and Hull's check of the SQL text is the
only separation - enough on SQLite (which also has an authorizer), but SQL
that builds a table name at run time can get past it on Postgres / MySQL, so
Hull logs a warning at startup there. Postgres sketch:

```sql
CREATE ROLE hull_internal LOGIN PASSWORD '...';
CREATE SCHEMA hull_internal AUTHORIZATION hull_internal;
ALTER ROLE hull_internal SET search_path = hull_internal;
REVOKE ALL ON SCHEMA hull_internal FROM PUBLIC;   -- the app role gets nothing
```

Outbox, inbox and jobs deliberately stay on the app's connection: their rows
must commit in the same transaction as the app's own writes. Search (its index
reads app tables) and the KV SQL store (on the connection you open it with)
stay there too. Those tables keep the SQL-text check only.

## Stdlib Middleware

### Middleware Factory Pattern

All middleware modules follow the same contract:

```lua
local mod = require("hull.<module>")
local mw = mod.middleware(opts)   -- factory returns a middleware function
-- mw signature: function(req, res) -> 0 | 1
--   0 = continue to next middleware / handler
--   1 = short-circuit (response already sent)
```

Middleware is **synchronous**. Keel runs the next middleware and the handler on
the connection as soon as it returns, so in JS:

- an `async` middleware, or any middleware returning a Promise / thenable, is an
  error: the request is answered 500 (its Promise used to coerce to `0`,
  "continue" - an awaited auth check let every request through);
- a Hull async op called from middleware (`hull.sleep`, `db.async.*`,
  `http.fetch`, `compute.async`, `gpu.async`, `worker.dispatch`, `smtp.send`)
  throws a `TypeError`, and a `req.multipart()` read that must wait for more
  body fails - do async work in the route handler.

Register with `app.use(method, pattern, mw)`:
- `"*"` method = match any method
- `"/*"` pattern = prefix match all paths
- `"/api/*"` = prefix match under `/api/`

### Module Reference

| Module | Lua | JS | Purpose |
|--------|-----|-----|---------|
| `cors` | `hull.web.middleware.cors` | `hull:web:middleware:cors` | CORS headers + preflight handling |
| `ratelimit` | `hull.web.middleware.ratelimit` | `hull:web:middleware:ratelimit` | In-memory rate limiting with configurable windows |
| `csrf` | `hull.web.middleware.csrf` | `hull:web:middleware:csrf` | Stateless CSRF token generation/verification |
| `auth` | `hull.web.middleware.auth` | `hull:web:middleware:auth` | Session-based and JWT-based authentication middleware |
| `oauth` | `hull.web.middleware.oauth` | `hull:web:middleware:oauth` | OIDC / OAuth 2.0 Authorization Code + PKCE (Google, Microsoft, generic IdPs) |
| `totp` | `hull.web.middleware.totp` | `hull:web:middleware:totp` | RFC 6238 TOTP 2FA with QR enrollment, recovery codes, replay-protection, optional at-rest encryption |
| `auth-flows` | `hull.web.auth-flows` | `hull:web:auth-flows` | Registration, email-verify, login, password-reset, magic-link, email-change. HMAC-signed single-use tokens, PBKDF2, app-provided storage + templates |
| `envelope` | `hull.crypto.envelope` | `hull:crypto:envelope` | HMAC-signed JSON-payload stateless tokens (`base64url(payload) "." hex(HMAC)`). Used internally by `hull/web/auth-flows` and `hull/web/middleware/oauth`; available standalone for any app that wants a tamper-detectable signed envelope without DB state |
| `crypto` | `hull.crypto` | `hull:crypto` | Primitives: `sha256`/`sha512`/`sha1`, `hmac_sha256` (+`_verify`), `hmac_sha1`, `hash_password`/`verify_password` (PBKDF2), `ed25519_*`, `verify` and `sign` (RS256/384/512, PS256, ES256/384 over a PEM key; ECDSA as raw r||s, RSA modulus-length), `rsa_private_pem(n, e, d, p, q)` (JS `rsaPrivatePem`: a PKCS#1 PEM from RSA components, CRT values derived, key checked), `key_from_env(var)` (JS `keyFromEnv`: a 32-byte secretbox key read from an allowlisted environment variable into C-owned memory; the handle seals and opens but never yields the bytes), `x25519`, `secretbox`/`box`, `auth`, `random`, `random_token(n [, "hex"])` (JS `randomToken`: `n` random bytes as unpadded base64url or hex, for ids, nonces and secrets), and `constant_time_eq(a, b)` for comparing secrets (never `==`); Lua also has the SSH set (`gcm_seal`/`gcm_open`, `aes256ctr`, `chacha20(key, nonce12, counter, data)` and `poly1305(key, msg)` (RFC 8439, raw: the caller composes the AEAD), `bcrypt_pbkdf`). **Bytes in, bytes out:** keys, nonces, signatures, tags, digests and ciphertexts are raw bytes of their exact size (a Lua string; a JS `ArrayBuffer` out, any buffer in), never hex; keypairs are `pk, sk` in Lua and `{ publicKey, secretKey }` in JS. For hex or base64, wrap with `hull.encoding` (`encoding.hex.encode(crypto.sha256(s))`). `hash_password`'s stored `pbkdf2:...` string is the one text format. **Bytes vs text:** a Lua string is its bytes; in JS pass bytes as a buffer (`encoding.bytes.toU8(s)`), since a JS string is taken as text and hashed as its UTF-8 |
| `encoding` | `hull.encoding` | `hull:encoding` | Byte <-> text codecs: `hex`, `base64` (`{ url = true }` for base64url), `base32`, `utf8` (text <-> UTF-8 bytes, strict), and `url` (RFC 3986 percent-encoding: `url.encode(s, { keep = "..." })`, `url.decode(s, { form = true })` reads `+` as a space and leaves a value with a malformed escape as written); strict decoding by default (`{ lenient = true }` skips whitespace). A refused decode is `nil, reason` in Lua (`bad_length`, `invalid_char`, `bad_padding`, `non_canonical`, `invalid_utf8`); in JS `decode` returns `null` and `why(same args)` gives the same reason. JS also has `bytes.toU8` / `bytes.fromBuffer`. See [encoding_consolidation_plan.md](encoding_consolidation_plan.md) |
| `hkdf` | `hull.crypto.hkdf` | `hull:crypto:hkdf` | HKDF-SHA256 (RFC 5869): `derive(ikm, length, { salt?, info? })`, plus `extract(salt, ikm)` / `expand(prk, info, length)`. Several independent keys from one high-entropy secret, each bound to its `info` label (at most 8160 bytes). Not a password hash (`crypto.hash_password` is). Lua returns a byte string, JS an ArrayBuffer |
| `otp` | `hull.crypto.otp` | `hull:crypto:otp` | HOTP (RFC 4226): `hotp(key, counter, digits?)` and `step(now, period?)` for TOTP (RFC 6238). The algorithm under `hull/web/middleware/totp` |
| `sealbox` | `hull.crypto.sealbox` | `hull:crypto:sealbox` | Versioned secretbox sealing under a keyring (`keyring` / `seal` / `open`), with an optional context bound into the sealed frame. Keys may be 32-byte strings or `crypto.key_from_env` handles; `keyring_from_env{ keys = { [1] = "VAR" }, current = 1 }` (JS `keyringFromEnv`) builds a keyring of held keys. `open` returns `value, version` or `nil, reason` in Lua and `[value, null, version]` or `[null, reason]` in JS (reason `unknown_version` or `open_failed`). Backs `hull/kv`'s `encrypt` option (see [kv_cache.md](kv_cache.md#encryption-at-rest)) and TOTP's encrypted secrets |
| `pwned` | `hull.web.pwned` | `hull:web:pwned` | k-anonymity pwned-password check via HIBP range API. Hashes the password SHA-1 client-side, sends only the first 5 hex chars over the wire, scans the returned suffix list locally. Apps must add `api.pwnedpasswords.com` to `manifest.hosts`. Fail-open on HIBP outage (a network failure or non-200); the runtime refusing the wait (called with a transaction open, or from middleware), and a `manifest.hosts` that certainly does not admit the endpoint, are raised, not taken for an outage. Used internally by `hull/web/auth-flows` when `check_pwned_passwords = true` |
| `audit-log` | `hull.web.middleware.audit-log` | `hull:web:middleware:audit-log` | Append-only sign-in / auth event log + per-device grouping. `record(user_id, kind, req, opts)`, `list(user_id, opts)`, `list_devices(user_id, opts)`, `is_new_device(user_id, req, opts)`. Fingerprint = `sha256(family_os|ip_prefix)[:16]`. Owns `_hull_audit_log`. Composes with auth-flows (emits events when `sign_in_log = true`), with session (per-device summary via `list_devices`), or standalone for app-recorded kinds (`api_token_issued`, `admin_impersonate`, etc.) |
| `session` | `hull.web.middleware.session` | `hull:web:middleware:session` | Server-side sessions backed by SQLite |
| `logger` | `hull.web.middleware.logger` | `hull:web:middleware:logger` | Request logging with logfmt output and request IDs |
| `transaction` | `hull.web.middleware.transaction` | `hull:web:middleware:transaction` | Wraps handlers in `db.batch()` (BEGIN IMMEDIATE..COMMIT) |
| `idempotency` | `hull.web.middleware.idempotency` | `hull:web:middleware:idempotency` | Idempotency-Key middleware with response caching |
| `outbox` | `hull.web.middleware.outbox` | `hull:web:middleware:outbox` | Transactional outbox for reliable side-effect delivery |
| `inbox` | `hull.web.middleware.inbox` | `hull:web:middleware:inbox` | Inbox deduplication for incoming events/webhooks |
| `cookie` | `hull.web.cookie` | `hull:web:cookie` | Cookie parse/serialize helpers |
| `jwt` | `hull.jwt` | `hull:jwt` | JWT sign/verify (HMAC-SHA256) |
| `template` | `hull.template` | `hull:template` | HTML template engine with inheritance, includes, filters |
| `validate` | `hull.validate` | `hull:validate` | Declarative input validation with schema rules |
| `form` | `hull.web.form` | `hull:web:form` | URL-encoded form body parsing |
| `i18n` | `hull.i18n` | `hull:i18n` | Internationalization: locale detection, translations, formatting |
| `csv` | `hull.csv` | `hull:csv` | CSV parse/encode (RFC 4180) |
| `tar` | `hull.archive.tar` | `hull:archive:tar` | ustar archive parse/create/extract/pack (extract/pack compose the fs capability) |
| `qrcode` | `hull.qrcode` | `hull:qrcode` | QR Code generator (ISO/IEC 18004), pure Lua/JS |
| `search` | `hull.search` | `hull:search` | Full-text search (SQLite FTS5) |
| `kv` | `hull.kv` | `hull:kv` | Portable key/value STORE: `open{backend,namespace}` -> handle (`get/set/delete/exists/incr/cas/scan/clear/cleanup/stats/caps`). Backends: memory / sqlite / postgres (over an existing `hull/db` conn). Durable, no eviction unless asked; binary-safe bytes. See [docs/kv_cache.md](kv_cache.md). Optional `encrypt` keyring seals values at rest |
| `cache` | `hull.cache` | `hull:cache` | In-process value memoizer (`cache.new`/`get/set/fetch`) PLUS `cache.open{backend,namespace,max_bytes,default_ttl}` -> byte-oriented, LRU-evicting CACHE handle (memory / sqlite). Ephemeral + bounded; distinct from `hull/kv` (see [docs/kv_cache.md](kv_cache.md)) |
| `rbac` | `hull.web.middleware.rbac` | `hull:web:middleware:rbac` | Role-based access control |
| `health` | `hull.web.middleware.health` | `hull:web:middleware:health` | Health check + readiness endpoints |
| `etag` | `hull.web.middleware.etag` | `hull:web:middleware:etag` | ETag response helpers with 304 Not Modified |
| `db.udf` | `db.udf.register/unregister` | `db.udf.register/unregister` | User-defined SQL functions (Lua/JS callbacks or WASM) |
| `image` | `hull.image` | `hull:image` | Image decode/encode (stb_image), raw pixel buffers |
| `ws-server` | `hull.web.ws-server` | `hull:web:ws-server` | WebSocket server (`app.ws`, `broadcast`, `connections`) |
| `ws-client` | `hull.web.ws-client` | `hull:web:ws-client` | WebSocket client (`ws.connect`) |
| `sse` | `hull.web.sse` (decorates `app.sse`) | `hull:web:sse` (decorates `app.sse`) | Server-Sent Events |
| `json` | `hull.json` | (built-in) | JSON encode/decode |

### Module APIs

**cors.middleware(opts)**. CORS headers + OPTIONS preflight.
- `opts.origins`. List of allowed origins (default: `{"*"}`)
- `opts.methods`. Allowed methods string (default: `"GET, POST, PUT, DELETE, OPTIONS"`)
- `opts.headers`. Allowed headers string (default: `"Content-Type, Authorization"`)
- `opts.credentials`. Boolean, send `Access-Control-Allow-Credentials` (default: `false`)
- `opts.max_age`. Preflight cache seconds (default: `86400`)
- Returns `1` on OPTIONS preflight (sends 204), `0` otherwise.

**ratelimit.middleware(opts)**. Per-key request rate limiting (in-memory, resets on restart).
- `opts.limit`. Max requests per window (default: `60`)
- `opts.window`. Window in seconds (default: `60`)
- `opts.key`. String or `function(req) -> string` (default: per client IP). The function must return a string or a number synchronously; anything else (a table / object, a Promise from an `async` key function) raises, since a fresh object per request was a fresh bucket and nothing was limited.
- Sets `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset` headers.
- Returns `1` on limit exceeded (sends 429 + JSON), `0` otherwise.

**csrf.middleware(opts)**. Stateless CSRF protection using HMAC tokens.
- `opts.secret`. HMAC secret (required)
- `opts.session_key`. Key in `req.ctx` for session ID (default: `"session_id"`) [Lua]
- `opts.max_age`. Max token age in seconds (default: `3600`)
- `opts.header_name`. Header to read token from (default: `"x-csrf-token"`)
- `opts.field_name`. Form field name (default: `"_csrf"`)
- `opts.safe_methods` / `safeMethods`. Methods that skip verification (default: `{"GET","HEAD","OPTIONS"}`; JS honours it too since audit 6).
- Safe methods: generates token → `req.ctx.csrf_token`.
- Unsafe methods: verifies token from header or form field.
- Returns `1` on verification failure (sends 403 + JSON), `0` otherwise.
- Helpers: `csrf.generate(session_id, secret)`, `csrf.verify(token, session_id, secret, max_age)`.
- **Body size caps (bounded work per request):** when reading the CSRF
  token from a url-encoded form body, the middleware caps total body at
  **1 MiB**, max **256** form pairs, and (Lua) max **4 KiB** per individual
  pair. Requests exceeding the body cap get **413**. Large multipart
  uploads should use a multipart parser BEFORE `csrf.middleware` in the
  stack. By the time CSRF sees the body, it should already be the
  pre-parsed url-encoded form, not the raw upload.

**auth.session_middleware(opts)**. Session cookie authentication.
- `opts.cookie_name`. Session cookie name (default: `"hull_session"`)
- `opts.optional`. Continue without session (default: `false`)
- `opts.login_path`. Redirect here on failure instead of sending 401
- Sets `req.ctx.session` and `req.ctx.session_id`.
- Returns `1` on auth failure (sends 401 or redirect), `0` on success.

**auth.jwt_middleware(opts)**. JWT Bearer token authentication.
- `opts.secret`. HMAC-SHA256 secret (required)
- `opts.optional`. Continue without token (default: `false`)
- `opts.require_exp` / `requireExp` (default `true`). A token without an
  `exp` claim is refused; pass `false` to accept non-expiring tokens.
  (Before audit 4 the default was `false`, so `jwt.sign` without `exp`
  minted a token the middleware honoured forever.)
- Reads `Authorization: Bearer <token>` header.
- Sets `req.ctx.user` (decoded payload).
- Returns `1` on auth failure (sends 401 + JSON), `0` on success.

**auth.login(req, res, user_data, opts)**. Creates session, sets cookie. Returns `session_id`. `opts.ttl`, when given, must be a positive number of seconds (it bounds both the cookie and the session); `0` or a negative value raises in both runtimes.
`opts.ttl` bounds both: the cookie's `Max-Age` and the session itself, which
slides by that ttl on every load rather than by the module TTL
(`session.create(data, { ttl })` stores a per-session ttl).

**auth.logout(req, res, opts)**. Destroys session, clears cookie.

**oauth**. OIDC / OAuth 2.0 Authorization Code flow with PKCE. Owns three
routes (`/auth/:provider/login`, `/auth/:provider/callback`, `/auth/logout`);
verifies ID token signatures against the IdP's JWKS (x5c → SPKI PEM via
`crypto.x509_pubkey_pem`); HMAC-signs the per-request state + nonce + PKCE
verifier into an HttpOnly cookie so the callback can't be replayed
cross-provider or by a CSRF.

- `oauth.init(opts)`. Call once at app startup. Required: `state_secret`
  (≥16 bytes), `providers = { name = {...}, ... }`. Optional: `state_cookie`,
  `state_ttl`, `find_user(provider, claims) -> user` (required when
  `on_login` is set), `on_login(req, res, user, ctx) -> path?` (signature
  matches `hull/web/auth-flows` so a single `session.login_handler(cookie)`
  wires both; `ctx = { provider, claims, tokens }` exposes OIDC-specific
  data for callers that need it), `on_logout(req, res) -> path?`.
- `oauth.routes(app)`. Mounts the three routes on the given app.
- Provider config. Either `preset = "google" | "microsoft"` (plus
  `client_id`, optional `client_secret`, `scopes`, `tenant` for Microsoft:
  `common` accepts any tenant's issuer, `consumers` only the personal-
  account tenant's, `organizations` any tenant but that one, a GUID or
  domain only its own)
  or fully-explicit `{ authorization_endpoint, token_endpoint, jwks_uri,
  issuer, client_id, client_secret?, scopes? }`.
- Allowed signing algs: `RS256 / RS384 / RS512 / PS256 / ES256 / ES384`
  (HS256 excluded - OIDC IdPs don't sign ID tokens with HMAC).
- App must declare `hull/web/middleware/oauth@1` in `manifest.modules` plus
  add the IdP host (e.g. `accounts.google.com`, `login.microsoftonline.com`)
  to `manifest.hosts`.
- JS API: same shape with camelCase keys (`stateSecret`, `clientId`,
  `clientSecret`, `onLogin`, `onLogout`).
- See `tests/fixtures/oauth_client_lua/app.lua` for a complete worked
  example, exercised end-to-end against a Python mock IdP via
  `tests/e2e_oauth.sh`.

**totp**. RFC 6238 Time-based One-Time Password 2FA. Composes with
the existing `auth` + `session` modules - after password verify, the
app sets `req.ctx.session.pending_2fa = true`; this module's
middleware gates sensitive routes until a valid TOTP code (or recovery
code) is presented. Algorithm is fixed at HMAC-SHA1 (RFC 6238 default,
what every mainstream authenticator app - Google Authenticator,
Authy, 1Password - supports).

- `totp.init(opts)`. Required at app startup. Creates `_hull_totp` +
  `_hull_totp_recovery` tables.
  - `opts.issuer` (default `"Hull"`) - label shown in authenticator.
  - `opts.digits` (default `6`; also accepts `8`).
  - `opts.period` (default `30s`, RFC default). Applies to new enrolments;
    an existing one keeps the period it was enrolled with.
  - `opts.window` (default `±1` step → ~90s clock-skew tolerance).
  - `opts.recovery_codes` (default `10`).
  - `opts.max_failed_attempts` (default `5`), `opts.lockout_duration`
    (default `900`s), `opts.max_lockout_duration` (default `86400`s): every
    `max_failed_attempts` wrong codes lock the user out, each lockout twice
    as long as the last, up to the cap (`0` = no escalation, in both
    runtimes). A successful verify or a confirmed (re-)enrolment resets
    it. `opts.on_lockout(user_id, locked_until, n)` (`onLockout`) is
    called when a lockout starts, so the app can tell the user: someone
    holding the password can otherwise keep them out of TOTP and recovery
    codes without notice.
  - `opts.encryption_key` - optional 32-byte string. When set,
    secrets are NaCl-secretbox-encrypted at rest with a fresh nonce
    per enrollment. Caller manages the key (env, fs.read, etc.).
- `totp.enroll(user_id)` → `{ secret_base32, otpauth_url, qr_svg,
  recovery_codes }`. Recovery codes are returned ONCE; only PBKDF2
  hashes persist. Re-enrolling overwrites - intentional, it's the
  recovery path when both authenticator and codes are lost.
- `totp.confirm(user_id, code)` → bool. Pairs the authenticator;
  flips the row's `confirmed` flag.
- `totp.verify(user_id, code)` → bool. Bare boolean for the common
  `if not totp.verify(...) then deny() end` shape; matches JS. Use
  `totp.verify_with_kind(user_id, code)` → `(ok, "totp"|"recovery"|nil)`
  when you need the kind for audit metadata. TOTP path first;
  replay-protected via `last_used_step` (atomic compare-and-set in
  the UPDATE WHERE). Recovery-code path scans unused rows; consumed
  codes get `used_at` stamped.
- `totp.disable(user_id)` - deletes secret + recovery codes.
- `totp.enrolled(user_id)` → bool. Confirmed enrollment check.
- Login-time 2FA gating happens via `hull/web/auth-flows`
  (enable_totp + user_totp_enrolled + totp_verify callbacks). There
  is no `totp.middleware` / `pending_2fa` mechanism - the auth-flows
  envelope path is the single supported way.
- JS API mirrors the Lua surface with camelCase
  (`verifyWithKind` / `secretBase32` / `otpauthUrl` / `qrSvg` /
  `recoveryCodes` in the enroll return).
- Local-first note: TOTP needs no network at verify time (works
  air-gapped). Clock skew matters more off-cloud - raise `opts.window`
  on devices without NTP.

**auth-flows**. Transactional auth-flow recipes. Bundle of routes that
cover registration / email-verify / login / password-reset / magic-link
/ email-change. The module owns the auth-internal bookkeeping tables
(`_hull_auth_used_tokens`, `_hull_auth_pending_email_changes`) but
does NOT own the users table - the app provides `user_*` callbacks so
existing user models drop in. Optional TOTP composition wedges a 2FA
verify step between successful first-factor auth and `on_login` when
`enable_totp = true` is set (see below).

- `authflows.init(opts)`. Required at app startup. Validates all
  callbacks up front + creates internal tables.
  - `opts.state_secret` (≥32 bytes). HMAC key for the signed tokens.
  - `opts.email_send(to, subject, html, text)`. App-provided.
  - `opts.templates = { welcome, verify, magic_link, password_reset,
    email_change }`. Each is `function(ctx) → { subject, html?, text? }`.
  - `opts.user_*` callbacks (find_by_email, get, create, set_password,
    set_email, set_email_verified). All required; missing ones are
    reported together at init time. **Shortcut:** pass
    `opts.users = authflows.standard_users({ table = "users" })`
    to bulk-fill all six against the standard schema in one line;
    any explicit `opts.user_X` still overrides. The adapter is
    DB-backend-agnostic (works on whatever backend `hull/db` is
    wired to). Apps with a custom schema either pass the 6
    callbacks directly or post-process the adapter table.
    `user_find_by_email` must return `password_hash` (login and the
    verify step read the hash through it); `user_get` need not, and an
    account whose hash cannot be read that way is treated as having a
    password (the verify step is shown, never skipped).
    `email_verified` is a boolean; `0` / `1` from a raw row are read as
    false / true in both runtimes. Anything else (a string `"0"` /
    `"1"` / `"true"` from a TEXT column) reads as NOT verified in both.
  - `opts.on_login(req, res, user)` / `opts.on_logout(req, res)`. App
    issues its own session (cookie, JWT, whatever) here. Module is
    session-agnostic. **Shortcut:** wire
    `opts.on_login = session.login_handler(cookie)` and
    `opts.on_logout = session.logout_handler(cookie)` to get
    session creation + session-fixation defense + Set-Cookie + JSON
    response in one line each. The same factories work for OAuth
    after the find_user split (below).
  - `opts.require_verified_email` (default `true`). Block login until
    email is verified. Opt-out for apps that gate per-route on the
    `email_verified` flag instead. Opting out REQUIRES
    `opts.on_password_reset` (init raises without it): unverified
    accounts can then sign in, so a pre-registrant can hold a session
    when the address owner sets the password at verification, and the
    app's `on_password_reset` (e.g. `session.destroy_all(user.id)`) is
    the only place those sessions can be revoked. Pass a no-op function
    if the app keeps no sessions.
  - `opts.magic_link_auto_signup` (default `false`). Silent no-op
    when magic-link is requested for an unknown email (enumeration-
    safe). Opt-in to auto-create a passwordless user instead.
  - `opts.enumeration_safe` (default `true`). Register / reset /
    magic-link / email-change return identical success shapes
    regardless of whether the email exists.
  - `opts.verify_ttl` (86400s), `opts.reset_ttl` (3600s),
    `opts.magic_link_ttl` (600s), `opts.email_change_ttl` (86400s).
  - `opts.prefix` (default `"/auth"`).
  - Emailed links need an origin: `opts.public_origin` (one canonical
    URL), or `opts.trusted_hosts` (the request's Host must match an entry
    exactly; the link is built from that entry, never from the request,
    so a bare host gives a link without a port and an entry
    `"host:port"` - which matches a request on that port only - gives
    that port), or `opts.trust_request_host = true` (dev / tests only:
    the request's host and port as sent).
  - `opts.enable_totp` (default `false`). Opt in to TOTP-as-second-
    factor on successful password login OR magic-link click. Requires
    `opts.user_totp_enrolled(user_id) -> boolean` and
    `opts.totp_verify(user, code) -> boolean`. Apps typically delegate
    to `hull/web/middleware/totp` from these two callbacks; the
    recovery-code path flows transparently because `totp.verify`
    accepts both 6-digit and recovery codes.
  - `opts.totp_pending_ttl` (default `300`). Lifetime of the pending-
    2FA token issued between first-factor success and `/totp-verify`.
  - `opts.totp_pending_redirect`. When set, magic-link sign-ins (the
    page's form POST) that require 2FA redirect to
    `<redirect>?token=<totp_token>` instead of rendering the module's
    default minimal HTML form. `/login`, and a JSON magic-link POST,
    always respond with JSON
    `{ ok: true, pending_2fa: true, totp_token: "…" }`.
  - **Hardening options** (all opt-in via init):
    - `opts.max_failed_logins` (default `5`) + `opts.lockout_duration`
      (default `900` = 15 min). After N consecutive failed-password
      attempts **from one client IP**, that (account, IP) pair trips a
      `locked_until` window in `_hull_auth_login_attempts`; login then
      fails during the window regardless of whether the submitted
      password is right (the same 401 as a wrong password, so the lock
      does not reveal that the account exists). Keyed on the account
      alone, five wrong passwords from anywhere locked anyone out.
      `opts.max_failed_logins_per_account` (default `50`;
      `maxFailedLoginsPerAccount` in JS) is a second, account-wide count
      that still stops a brute force spread over many addresses. The
      client IP honours `trust_proxy`. Counters clear on successful login
      (that address and the account-wide one) or `password-reset/confirm`
      (all of them).
    - Registration, verify-resend, magic-link and password-reset requests
      issue their token and send their email **after** the response, on the
      event loop: those steps happen only for some addresses, so doing them
      inline let response time say whether an account exists. A failing
      `email_send` is logged instead of failing the request.
    - `opts.check_pwned_passwords` (default `false`). Routes
      register + password-reset-confirm through `hull/web/pwned`
      (HIBP k-anonymity). Apps must add `api.pwnedpasswords.com`
      to `manifest.hosts`: a hosts list that does not admit the
      endpoint raises (500), it is not taken for an outage. Fail-open
      on HIBP outage. Tests can override the endpoint via
      `opts.pwned_endpoint`.
    - **Email-change notify+revoke** activates implicitly when the
      app provides a `templates.email_change_notify` template. A
      revoke link is sent to the OLD address on every email-change
      request; the OLD-address holder can click it (and submit the
      page it opens) to delete the pending change
      (`/email-change/revoke?token=…`) within
      `email_change_ttl` even if the attacker holds a valid
      session cookie.
    - `opts.sign_in_log` (default `false`). Routes every login /
      password-reset-completed / email-changed / email-change-
      revoked into `hull/web/middleware/audit-log` so apps can
      surface a per-user device list, recent events, and "you
      signed in from a new device" UX. Requires
      `hull/web/middleware/audit-log` to be in scope (it's a
      transitive dep of auth-flows so the resolver auto-admits).
    - `opts.on_new_device(req, res, user)` (optional). Fires
      from inside the login path when `audit_log.is_new_device`
      returns true for this user + request fingerprint. App
      typically sends a "you signed in from a new device" email.
    - `opts.on_password_reset(req, res, user)` (optional). Fires
      after a successful `password-reset/confirm` updates the
      hash, and after a verify with `new_password`. May be async in JS
      (it is awaited; a throw or rejection is logged). Recommended implementation:
      `function(req, res, user) session.destroy_all(user.id) end`
      - revokes every existing session because a reset is the
      standard recovery move after a suspected compromise.
- `authflows.routes(app)`. Mounts the routes under `prefix`:
  POST `/register`, GET + POST `/verify`, POST `/verify/resend`,
  POST `/login`, POST `/logout`, POST `/magic-link`,
  GET + POST `/magic-link/consume`, POST `/password-reset/request`,
  POST `/password-reset/confirm`, POST `/email-change`,
  GET + POST `/email-change/confirm`, GET + POST `/email-change/revoke`,
  POST `/totp-verify` (404s when `enable_totp` is off).
  **A mailed single-use link is consumed by POST, never by GET.** Mail
  scanners prefetch every link: a GET that consumed a magic link signed
  the scanner in (the user's own click then answered "replayed"), and one
  that consumed an email-change confirm or revoke link changed or
  cancelled the change with nobody reading it. The GET of
  `/magic-link/consume`, `/email-change/confirm` and `/email-change/revoke`
  checks the token without using it and renders a one-button page (no
  script; `secure_html` headers) whose form POSTs `token` back to the
  same path; a JSON client POSTs `{token}` itself and gets JSON back. A
  magic-link POST marked `Sec-Fetch-Site: cross-site` is refused (403:
  it would sign the victim in to the attacker's account), and a
  confirmed email change answers like `/verify` (a 303 to
  `verify_redirect`, or `{ok, redirect}` for JSON). An app with a
  global CSRF middleware must exempt these POSTs, as it does `/verify`.
  `/verify/resend` is enumeration-safe - always returns `{ok:true}`
  whether the user exists, is unverified, or is already verified.
  Apps SHOULD rate-limit it (per-email key) to bound mail volume.
  **Email verification is two steps, and the owner always chooses.**
  Anyone can register any address and choose its password, so a click on
  the welcome (or resend) link proves only that the clicker reads the
  mailbox. `GET /verify?token=` never consumes the token - mail scanners
  (Safe Links, Proofpoint, Mimecast) prefetch every link - and renders a
  small form (no script; `secure_html` headers), or redirects to
  `opts.verify_form_redirect` (`verifyFormRedirect`) with `?token=`
  appended so the app renders its own. The form, or the app's page, POSTs
  to `/verify` (form-encoded or JSON; JSON gets JSON back, a form gets a
  303 to `verify_redirect` or the page again):
  - `{token, password}`: the password must match the account's. On a
    match the address is verified and the password kept. A wrong one is a
    401 (`{error, new_password_allowed: true}`) that leaves the token
    usable and counts toward the login lockout (the same 401 while the
    account is locked), so this is no better a password oracle than
    `/login`.
  - `{token, new_password}` (length + pwned rules as `/register`): proves
    the mailbox only, so the password is replaced and everything a
    pre-registrant could have attached goes with it - a pending email
    change, the lockout rows, a TOTP enrolment (through
    `opts.totp_disable(user_id)`, typically `totp.disable`; without the
    hook an existing enrolment is only logged and `on_password_reset` must
    remove it), and sessions (`on_password_reset`). Verified is set last.
  An already-verified account just consumes the token. A magic link to an
  unverified account that has a password shows the same verify step
  instead of signing in (a passwordless account is simply verified), and
  a password reset of an unverified account verifies it the same way as
  `new_password` does. Nothing is replaced silently. A password reset
  also drops a pending email change, and an email-change confirm link
  must be the pending change's latest one. `standard_users` refuses a
  `_hull_*` table name. Addresses must be a single address: one `@`, none
  of `, ; < > " ( )` or whitespace.
- `authflows.send_verify_email(user, url_prefix)`,
  `authflows.send_password_reset(email, url_prefix)`,
  `authflows.send_magic_link(email, url_prefix)`. Standalone helpers
  for admin/programmatic triggers (resend, etc.).
- Token format: `base64url(JSON{sub, action, exp, nonce}) "." hmac_hex`.
  Signature framing comes from `hull/crypto/envelope` (shared with
  the OAuth state cookie); auth-flows layers single-use enforcement
  via `_hull_auth_used_tokens` (sha256 of full token as PK, atomic
  INSERT OR IGNORE → 0 rowcount = replay) plus action-tag + expiry
  checks. The TOTP-pending flow uses the underlying envelope.verify
  directly so the token stays usable across retry-on-typo attempts
  and is only burned on a successful code verify.
- JS API: camelCase keys (`stateSecret`, `emailSend`, `userFindByEmail`,
  `onLogin`, `magicLinkAutoSignup`, `requireVerifiedEmail`, `enableTotp`,
  `userTotpEnrolled`, `totpVerify`, `totpPendingTtl`, `totpPendingRedirect`,
  `verifyFormRedirect`, `totpDisable`). The `user*` lookups, `totpVerify` and
  `userTotpEnrolled` must answer synchronously (a Promise is a 500 / fails
  closed); `onLogin`, `onPasswordReset` and `totpDisable` may be async and
  are awaited.
  `totp.verify(userId, code)` returns a bare boolean (the historical
  `[ok, kind]` tuple lives behind `totp.verifyWithKind` now - see
  the TOTP section), so a `totpVerify: (user, code) => totp.verify(
  user.id, code)` delegate is safe by default.
- Email-change flow re-verifies on the NEW address - old email stays
  active until the user clicks the link sent to the new one.

**session**. Server-side sessions backed by SQLite. Requires `session.init()` at startup.
- `session.init(opts)`. Creates `hull_sessions` table; runs PRAGMA-checked additive migrations (adds `user_id`, `ip`, `user_agent` columns for the device-management helpers). `opts.ttl` = lifetime in seconds (default: `86400`).
- `session.create(data, opts?)` → 64-char hex session ID. `opts.req` lets it capture ip + ua + user_id columns at create time. `data.user_id` is auto-populated into the column.
- `session.load(session_id)` → data table or nil. Auto-extends expiry.
- `session.update(session_id, data)`. Updates session data.
- `session.destroy(session_id)`. Deletes session.
- `session.cleanup()` → count of deleted expired sessions.
- **Device management** - `session.list_for_user(user_id)` → array of `{id, created_at, last_accessed, ip, user_agent}`; `session.destroy_others(current_sid, user_id)` → "sign out everywhere else"; `session.destroy_all(user_id)` → "sign out everywhere" (used by auth-flows on password reset cascade).
- **Login/logout factories** - `session.login_handler(cookie, opts?)` returns a turnkey `on_login(req, res, user, ctx?)` callback that creates a session, sets the cookie, and responds. Defaults to session-fixation defense (`session.rotate(prior_sid, ...)`). `opts.name` (cookie name, default `"hull_session"` - same as `auth.session_middleware`), `opts.cookie_opts` (forwarded to `cookie.serialize`), `opts.extract_data(user) -> data`, `opts.respond(res, user, sid)`, `opts.rotate` (default `true`), `opts.audit_log` (module ref - when set, records a login event after the session is set), `opts.audit_kind` (default `"login"`), `opts.audit_metadata(user, ctx) -> table` (default derives `{ factors = ctx.factors }` for auth-flows or `{ factors = "oauth:" .. ctx.provider }` for oauth), `opts.on_new_device(req, res, user)` (requires `audit_log` - called before record when `audit_log.is_new_device` returns true). `session.logout_handler(cookie, opts?)` is the matching `on_logout`; it answers 403 to a request the browser marks `Sec-Fetch-Site: cross-site` (as do auth-flows' `POST /logout` and oauth's logout), since the clearing `Set-Cookie` would sign the victim out. In JS both factories return what `respond` / an async `onNewDevice` returns, so an async callback is awaited. Same factories work for `hull/web/auth-flows` AND `hull/web/middleware/oauth` (the audit + new-device seam covers both for free).
- `session.rotate(old_sid, data, opts)` - destroy + recreate, session-fixation defense primitive. Used by `login_handler`; apps doing custom on_login can call it directly.

**The `on_login(req, res, user, ctx?)` contract.** Both `hull/web/auth-flows` and `hull/web/middleware/oauth` hand off through this single shape. Guarantees:
- `user` is the **app's** user object (whatever `find_user` / `standard_users` / a custom adapter produced). `user.id` MUST be a non-empty string - `session.login_handler` enforces it and throws otherwise. `user.email` is conventionally present but not required.
- `ctx` is `nil` for the simplest call sites, or a table carrying source-specific metadata. auth-flows passes `{ factors = "password" | "magic_link" | "password+totp" }`. oauth passes `{ provider, claims, tokens }` (the IdP claims + raw tokens, for apps that want to capture them).
- Returning a string overrides the post-login redirect target (oauth honors this; auth-flows uses the redirect from `login_redirect` opt).
- `on_logout(req, res)` is the matching shape - no `user` arg because the session row is the source of truth there.

**Audit-metadata scrub at the session.login_handler seam.** When `audit_log` is wired, the factory calls your `audit_metadata(user, ctx)` and then **strips** these keys from the result before passing it to `audit_log.record`: `tokens`, `token`, `access_token`, `refresh_token`, `id_token`, `claims`, `password`, `password_hash`, `pwhash`, `secret`. This is defense in depth - the **OAuth ctx already contains `claims` and `tokens`** (the raw IdP tokens), so a `audit_metadata = function(_,c) return c end` override would otherwise persist access_token + refresh_token in `_hull_audit_log.metadata` for `retain_days` (default 365). The scrub is top-level only; if you need to log claim details, pull them out by name in your custom `audit_metadata` (e.g. `return { factors = "oauth:" .. ctx.provider, sub = ctx.claims.sub }`) - never pass the raw `ctx` through.

**cookie**. Cookie helpers (not middleware).
- `cookie.parse(header)` → table `{ name = value, ... }`.
- `cookie.serialize(name, value, opts)` → `Set-Cookie` header string.
  - `opts.path` (default: `"/"`), `opts.httponly` (default: `true`), `opts.secure`, `opts.samesite` (default: `"Lax"`), `opts.max_age`, `opts.domain`.
- `cookie.clear(name, opts)` → `Set-Cookie` header with `Max-Age=0`.

**jwt**. JWT sign/verify (HS256 only, not middleware).
- `jwt.sign(payload, secret)` → token string. Auto-sets `iat`.
- `jwt.verify(token, secret)` → payload table, or `nil, "error reason"`.
  A token whose header lists `crit` extensions is refused (none are
  implemented; RFC 7515 §4.1.11).
- `jwt.decode(token)` → payload table or nil (no signature check).

**logger.middleware(opts)**. Request logging with logfmt output and auto-assigned request IDs.
- `opts.skip`. List of paths to skip (exact match, e.g. `{"/health"}`)
- `opts.include_headers`. List of header names to include in log line
- Sets `X-Request-ID` response header and `req.ctx.request_id`.
- Helpers: `logger.generate_id()`, `logger.format_line(entries)`, `logger.should_skip(path, skip_list)`.
- Returns `0` (always continues).

**validate.check(data, schema)**. Declarative input validation.
- `schema` maps field names to rule tables.
- Rules: `required`, `trim`, `type` (`"string"`, `"number"`, `"integer"`, `"boolean"`), `min`, `max`, `pattern`, `oneof`, `email`, `fn` (custom validator: return `true` / `nil` for valid, `false` for invalid - the error is `message` or "is invalid" - or a string error message; any other return fails closed), `message` (custom error).
- `min`/`max` apply to string length or numeric value depending on field type.
- Returns `(ok, errors)` where `errors` maps field names to error strings.

**form.parse(body)**. URL-encoded form body parsing.
- Decodes `application/x-www-form-urlencoded` format.
- Handles `+` → space and `%XX` percent-encoding. Last value wins for duplicates.
- Returns table `{ field_name = value, ... }` (empty table for nil/empty input).

**i18n**. Internationalization: locale detection, message bundles, formatting.
- `i18n.load(name, tbl)`. Register a locale with translations and format rules.
- `i18n.locale(name?)`. Get or set the active locale. **Process-global**: every request shares it, so a handler that sets it and then yields or awaits (`db.async`, `http.fetch`, a timer) can resume to another request's locale. Use it only for single-request code; pass the locale explicitly with `t_in` otherwise.
- `i18n.t(key, params?)` → translated string. Supports `${variable}` interpolation and dot-path keys.
- `i18n.t_in(locale, key, params?)` (JS `i18n.tIn`) → the same, in an explicit locale, without touching the active one - safe across a yield.
- `i18n.number(n)` → formatted number (locale-specific decimal/thousands separators).
- `i18n.date(timestamp)` → formatted date string.
- `i18n.currency(amount, code)` → formatted currency string (symbol + locale rules).
  NaN / infinities / amounts past 2^53 minor units fall back to a plain
  rendering instead of raising.
- `i18n.number_in(locale, n)`, `i18n.date_in(locale, ts)`,
  `i18n.currency_in(locale, amount, code)` (JS `numberIn` / `dateIn` /
  `currencyIn`) → the same in an explicit locale, safe across a yield.
- `i18n.detect(accept_language_header)` → best matching locale name or nil.

**transaction**. Wraps handlers in SQLite transactions.
- `transaction.middleware()`. Post-body middleware that sets `req.ctx._txn = true` for downstream use.
- `transaction.run(fn)`. Wraps `fn` in `db.batch()` (BEGIN IMMEDIATE → fn() → COMMIT, ROLLBACK on error).
  A `db.batch` inside another on the same connection runs in a SAVEPOINT of
  the outer transaction (DuckDB: joins it): its writes commit with the outer
  one and its error rolls back only its own writes. (It used to issue its
  own BEGIN / COMMIT, committing the caller's transaction early on Postgres
  and MySQL.) MySQL commits the open transaction around every DDL statement
  (even one that fails). In a top-level batch the backend then opens a new
  transaction for the rest of it (what ran before the DDL stays committed);
  in a nested batch it cannot - the outer batches' savepoints went with the
  commit - so the batches report their transaction lost and fail.
- `transaction.try(fn)` → `(ok, err)`. Like `run` but returns error instead of throwing.

**idempotency**. Idempotency-Key middleware with response caching.
- `idempotency.init(opts)`. Creates `_hull_idempotency_keys` table. `opts.ttl` = key lifetime in seconds (default: `86400`).
- `idempotency.middleware(opts)`. Post-body middleware intercepting POST (configurable via `opts.methods`).
  - `opts.header_name`. Header to read key from (default: `"idempotency-key"`).
  - `opts.get_principal`. `function(req) -> string` for per-user scoping (default: `"__anon"`).
  - Cache hit + same fingerprint → returns cached response (handler skipped).
  - Cache hit + different fingerprint → returns 409 Conflict.
  - Fingerprint: `SHA-256(method + path + body)`.
- `idempotency.respond(req, res, status, data)`. Sends response and caches it for replay.
- `idempotency.complete(req)`. Marks key as processed without caching response body.
- **Replay-header allowlist (security):** when `idempotency.respond` is
  called with `extra_headers`, only headers on a strict allowlist are
  emitted AND persisted to SQLite. Allowed: `Content-*`, `Location`,
  `ETag`, `Last-Modified`, `Cache-Control`, `Vary`, the stdlib's own
  `X-Request-ID` / `X-RateLimit-*` / `X-Idempotency-Replay`, plus
  `X-Content-Type-Options`, `X-Frame-Options`, `Strict-Transport-Security`,
  `Content-Security-Policy`, `Referrer-Policy`, `Permissions-Policy`.
  Anything else. Especially credential headers like `Set-Cookie`,
  `Authorization`, `X-Auth-*`, `X-API-Key`, `X-CSRF-*`,
  `X-Forwarded-Authorization`, `X-Amz-Security-Token`, etc.. Is dropped
  silently on BOTH the cache-write and replay paths. Set credential
  headers via a separate middleware (e.g. session.create) instead of
  passing them through `respond()`'s `extra_headers`.
- `idempotency.cleanup()` → count of deleted expired keys.

**outbox**. Transactional outbox for reliable side-effect delivery.
- `outbox.init(opts)`. Creates `_hull_outbox` table. `opts.max_attempts` (default: `5`).
- `outbox.enqueue(opts)`. Enqueue a delivery (call inside a transaction).
  - `opts.kind`. Delivery type (e.g. `"webhook"`, `"email"`).
  - `opts.destination`. Target URL or address.
  - `opts.payload`. Payload string.
  - `opts.headers`. JSON-encoded headers (optional).
  - `opts.idempotency_key`. Dedup key for delivery (optional).
- `outbox.flush(opts)`. Deliver pending items. Exponential backoff (`2^attempt * 10s`, capped at 1hr).
- `outbox.middleware()`. Sets `req.ctx._outbox_flush = true` for auto-flush.
- `outbox.stats()` → `{ pending, delivered, failed }` counts.
- `outbox.cleanup(max_age)`. Delete old delivered items.

**inbox**. Inbox deduplication for incoming events/webhooks.
- `inbox.init(opts)`. Creates `_hull_inbox_processed` table. `opts.ttl` = record lifetime (default: `86400`).
- `inbox.is_duplicate(message_id, source?)` → boolean. Default source: `"default"`.
- `inbox.mark_processed(message_id, source?, opts?)`. Record as processed.
- `inbox.check_and_mark(message_id, source?, opts?)` → boolean (true = duplicate, false = new + marked).
- `inbox.cleanup()` → count of deleted expired records.

**template**. HTML template engine with compile-once, render-many caching.

```lua
local template = require("hull.template")
template.render("pages/home.html", data)       -- load + compile + render (cached)
template.render_string(source, data)            -- compile from string + render
template.compile("pages/home.html")             -- returns compiled function
template.clear_cache()                          -- clear compiled function cache
```

Template syntax:
- `{{ var }}`. HTML-escaped output
- `{{ var.path }}`. Dot path lookup (nil-safe)
- `{{ var | filter }}`. Pipe filter (`upper`, `lower`, `trim`, `length`, `default: value`, `json`, `raw`)
- `{{{ var }}}`. Raw (unescaped) output
- `{% if var %}` / `{% elif var %}` / `{% else %}` / `{% end %}`. Conditionals
- `{% for item in list %}` / `{% for key, val in obj %}`. Iteration
- `{% block name %}` / `{% extends "parent.html" %}`. Template inheritance
- `{% include "partial.html" %}`. Include partials
- `{# comment #}`. Stripped from output

Templates are loaded from `app_dir/templates/` in dev mode. In built binaries, templates are embedded as byte arrays via `hull build` or `make APP_DIR=...`.

**JS API** (camelCase):
```javascript
import { template } from "hull:template";
template.render("pages/home.html", data);       // load + compile + render (cached)
template.renderString(source, data);             // compile from string + render
template.compile("pages/home.html");             // returns compiled function
template.clearCache();                           // clear compiled function cache
```

**Template engine details:**
- **Compilation:** Templates are parsed (lexer → recursive-descent parser → AST), then code-generated to native Lua/JS source and compiled via `luaL_loadbuffer` (Lua) or `JS_Eval` (JS). Compiled functions are cached. Compile once, render many.
- **XSS safety:** All `{{ }}` output is HTML-escaped by default (`& < > " '` → entities). Only `{{{ }}}` and `| raw` bypass escaping.
- **Dot paths are nil-safe:** `{{ user.address.city }}` returns empty string if any intermediate is nil/undefined. No errors.
- **For-loop variables are scoped:** Inside `{% for item in items %}`, `item` refers to the loop variable, not `data.item`.
- **Lua truthiness caveat:** In Lua, empty tables `{}` and `0` are truthy. Use a boolean flag like `has_items = #items > 0` when checking emptiness in `{% if %}`.
- **Filters:** `upper`, `lower`, `trim`, `length`, `default: "value"`, `json`, `raw`. Filters chain: `{{ name | trim | upper }}`.
- **Inheritance:** `{% extends "base.html" %}` loads parent, child overrides `{% block name %}` content. Multi-level inheritance supported. Circular extends detected.
- **Includes:** `{% include "partials/nav.html" %}` inlines the partial's AST. Included templates share the same data context.
- **Template directory:** Place templates in `app_dir/templates/`. Names are relative paths (e.g. `"pages/home.html"`, `"partials/nav.html"`, `"base.html"`).
- **CSP nonce:** No engine magic needed. Pass nonce as data: `template.render("page.html", { csp_nonce = nonce })`, use `<script nonce="{{ csp_nonce }}">` in template.

**csv.parse(text, opts?)**. Parse CSV text (RFC 4180).
- `opts.headers`. First row is header; returns objects (default: `false`)
- `opts.separator`. Field delimiter (default: `","`)
- Returns array of row arrays, or row objects if `headers = true`.

**csv.encode(rows, opts?)**. Encode rows as CSV text.
- `opts.headers`. Rows are objects; emit header row (default: `false`)
- `opts.separator`. Field delimiter (default: `","`)
- `opts.sanitize_formulas` (`sanitizeFormulas` in JS, default **`true`**).
  A cell beginning with `= + - @` (or a tab / CR) gets a leading `'`, so a
  spreadsheet opening the export treats it as text, not a formula or DDE
  call. Plain numbers (`-5`, `+3.2`, `1e-3`) are left alone. Pass `false`
  for output that is not meant for a spreadsheet.
- Returns CSV string.

**tar** (`hull.archive.tar` / `hull:archive:tar`). ustar (`.tar`) archive
handling backed by the shared C core (`cap/tar.c`) - the SAME core `hull tools
install` uses for signed bundles. Namespaced under `hull/archive/` as the
container-format family (a future `zip` would be a sibling; stream codecs like
gzip belong under a separate `hull/compress/`). `parse`/`create` are pure
byte<->table transforms (no authority); `extract`/`pack` COMPOSE the fs
capability, so they enforce `manifest.fs.write` / `fs.read` exactly like
`fs.write`/`fs.read` (path validation + allowlist), NOT the trusted install-path
extractor. `parse`/`create`/`extract` accept any buffer type (string /
`MappedBuffer` / `WasmBuffer` / `ArrayBuffer`) for the archive bytes.
- `tar.parse(bytes)` -> array of `{ name, data, size, mode, is_dir }` (JS:
  `isDir`; `data` is a Lua string / JS `ArrayBuffer`). Malformed / truncated /
  traversal-bearing archives return `nil, err` (Lua) / throw (JS).
- `tar.create(entries)` -> archive bytes (Lua string / JS `ArrayBuffer`). Each
  entry is `{ name, data?, mode?, is_dir? }` (JS: `isDir`; `mode` defaults 0644).
  An absolute or `..`-bearing member name is refused.
- `tar.extract(bytes, dest_dir)` -> `true` | `nil, err` (JS: throws on error).
  Writes each member under `dest_dir` via the fs capability (needs
  `manifest.fs.write`). Directory entries are skipped (file writes create their
  parents; empty dirs are dropped).
- `tar.pack(files)` -> archive bytes. `files` is an array of path strings or
  `{ path, name? }` tables; each is read via the fs capability (needs
  `manifest.fs.read`) and added as a member (name defaults to the path).

**qrcode**. QR Code generator (ISO/IEC 18004). Pure Lua / JS, byte
mode, all four EC levels, versions 1-40, all 8 mask patterns scored
per spec. Algorithm structure adapted from Project Nayuki's
MIT-licensed QR Code generator library; tables transcribed from
ISO/IEC 18004:2015 Annex E + Table 9 and cross-verified against
Python's `qrcode` library on 48 input / EC / mask combinations.

- `qrcode.encode(text, opts?)` → `{ matrix, size, version, ec_level, mask }`.
  Matrix is 1-indexed; cells are 0 (light) or 1 (dark).
  - `opts.ec_level` (Lua) / `opts.ecLevel` (JS). `"L"|"M"|"Q"|"H"`, default `"M"`.
  - `opts.mask`. `0..7` to force a specific mask; omitted runs the
    8-mask score-and-pick selector per spec 8.8.2.
- `qrcode.svg(text, opts?)` → SVG string.
  - `opts.scale`. Pixel size per module (default 4).
  - `opts.margin`. Quiet-zone modules around the QR (default 4).
  - `opts.dark` / `opts.light`. Colors (default `"#000"` / `"#fff"`).
    Pass `light: "none"` for transparent background.
- Input is treated as bytes (Latin-1 / ASCII). For non-ASCII payloads,
  encode to UTF-8 bytes at the call site before passing to `encode`.

Used by `hull/web/middleware/totp` for enrollment QR rendering;
also useful for WiFi codes, contact cards, payment links, etc.

**search**. Full-text search backed by SQLite FTS5.
- `search.create_index(name, columns, opts?)`. Create FTS5 virtual table.
- `search.index(name, id, fields)`. Insert/replace document.
- `search.remove(name, id)`. Delete document.
- `search.query(name, query, opts?)`. Full-text search. Returns `{id, rank}` array.
  - `opts.limit` (default: 20), `opts.offset` (default: 0)
- `search.reindex(name, source_table, opts?)`. Bulk re-index from table.
- `search.drop_index(name)`. Drop FTS5 table.

**rbac**. Role-based access control backed by SQLite.
- `rbac.init()`. Creates `_hull_roles`, `_hull_permissions`, `_hull_role_permissions`, `_hull_user_roles` tables.
- `rbac.define_role(name, permissions?)`. Create role with optional permissions.
- `rbac.assign(user_id, role)` / `rbac.revoke(user_id, role)`. Manage user roles.
- `rbac.roles(user_id)` → array of role names.
- `rbac.has_role(user_id, role)` → boolean.
- `rbac.has_permission(user_id, permission)` → boolean.
- `rbac.require_role(role)` → middleware function (403 on denial).
- `rbac.require_permission(perm)` → middleware function (403 on denial).

**health**. Liveness (`/health`) and readiness (`/ready`) endpoints with DB ping, custom checks, and server stats.
- `health.register(name, fn)`. Register a custom health check. `fn()` returns `true` or `false`. Checks are synchronous: in JS a check that returns a Promise fails (its Promise used to read as `ok`).
- `health.unregister(name)`. Remove a registered check.
- `health.run_checks(opts)` → `{ checks, all_ok }`. `opts.db_check` (default: `true`).
- `health.middleware(opts)`. Returns middleware that intercepts `/health` and `/ready`.
  - `opts.path_health`. Liveness path (default: `"/health"`). Returns `{ status: "ok", uptime }`.
  - `opts.path_ready`. Readiness path (default: `"/ready"`). Returns status, per-check status, uptime.
  - `opts.db_check`. Include DB ping (default: `true`).
  - `opts.details` (default `false`). Also return each check's error text and latency, and the server stats. `/ready` is normally unauthenticated, so the raw error strings (DB driver errors) and stats are opt-in; enable them only behind auth or on a private listener.
  - Returns `1` on health/ready paths, `0` otherwise (passes through to next handler).
  - Readiness returns 503 if any check fails.
- The DB ping uses the default connection in both runtimes. **JS:** `health.setDb(x)` (or `opts.db`) selects another: a connection (`dbModule.connect("name")`), or the `hull:db` module itself, whose default connection is then used.

**etag** (ETag response helpers. Not a middleware) provides wrapper functions for route handlers.
- `etag.json(req, res, data, status?)`. Send JSON response with ETag. Sends 304 if `If-None-Match` matches.
- `etag.text(req, res, text, status?)`. Same for text responses.
- `etag.html(req, res, html, status?)`. Same for HTML responses.
- `etag.compute(body)` → `W/"<first 16 hex chars of SHA-256>"` or `nil`.
- `etag.matches(req, tag)` → boolean. Checks `If-None-Match` header (comma-separated, `*` wildcard).
- Only computes ETags for GET/HEAD requests. Skips bodies > 1 MB.

**db.udf**. User-defined SQL functions backed by Lua/JS callbacks or WASM modules.
- `db.udf.register(name, fn, opts?)`. Register scalar UDF (Lua/JS function).
- `db.udf.register(name, {step, finalize}, opts?)`. Register aggregate UDF.
- `db.udf.register(name, "module_name", opts?)`. Register WASM-backed UDF.
- `db.udf.unregister(name)`. Remove a registered UDF.
- A UDF runs inside the statement that calls it, so it cannot `query` / `exec`
  on its own connection: such a call is refused ("a user-defined function
  cannot query its own connection"). Before, it could evict or reset the
  statement still being stepped.
- `opts.deterministic`. Boolean, enables SQLite optimizer (default: false)
- `opts.aggregate`. Boolean, WASM aggregate mode (default: false)
- `opts.args`. Number of arguments (-1 = variadic, default: 1)
- `opts.gas`. Per-row gas limit for WASM UDFs (default: 100K)
- Names must start with `hull_` to prevent shadowing SQLite built-ins.
- Lua/JS UDFs work with `db.query()` only (sync). WASM UDFs work with both `db.query()` and `db.async.query()`.

**image**. Image creation, encoding, and decoding via pluggable codec vtable (stb_image default).
- `image.new(w, h, format, pixels)` → HlImage. Formats: `"rgba8"`, `"r8"`, `"rgba16float"`, `"r32float"`.
- `image.from_buffer(buf, w, h, format)` → HlImage. Zero-copy borrow from a `MappedBuffer`/`WasmBuffer` (the source's bytes back the image directly). The borrow is refcounted: closing the source (`buf:close()`) while the image is alive is safe and defers the source's actual munmap/free until the last borrowing image is freed (no dangling pixels). Other sources (string, `ArrayBuffer`/typed array, another image) have no refcountable object to pin and are copied.
- `image.decode(data, format?)` → HlImage. Auto-detects PNG/JPEG/BMP from magic bytes.
- `image.encode(img, format, opts?)` → bytes. `opts.quality` for JPEG (default 90).
- `img:width()`, `img:height()`, `img:format()`, `img:size()`. Properties.
- `img:pixels()`. Raw pixel bytes.
- `img:close()`. Explicit free (GC handles it otherwise).

### WebSocket Endpoints

Register WebSocket endpoints with `app.ws(path, callbacks)`. Server-side connections are managed via the `HlWsRegistry`.

**Lua:**
```lua
app.ws("/ws/chat", {
    on_open = function(conn)
        log.info("connected: " .. conn:id())
    end,
    on_message = function(conn, msg, is_binary)
        ws.broadcast("/ws/chat", msg)
    end,
    on_close = function(conn, code, reason)
        log.info("disconnected: " .. conn:id())
    end,
})
```

**JavaScript:**
```javascript
import { wsServer } from "hull:web:ws-server";
app.ws("/ws/chat", {
    onOpen(conn) { log.info("connected: " + conn.id); },
    onMessage(conn, msg, isBinary) { ws.broadcast("/ws/chat", msg); },
    onClose(conn, code, reason) { log.info("disconnected: " + conn.id); },
});
```

**Connection object:**
- `conn:id()` / `conn.id`. Monotonic connection ID (getter)
- `conn:path()` / `conn.path`. Endpoint path (getter)
- `conn:send(text)` / `conn.send(text)`. Send text frame
- `conn:send_binary(data)` / `conn.sendBinary(data)`. Send binary frame
- `conn:close(code?, reason?)` / `conn.close(code?, reason?)`. Initiate close
- `conn:ping(data?)` / `conn.ping(data?)`. Send ping
- `conn.data`. Per-connection storage (table/object, lazy-created)

**Module functions:**
- `ws.broadcast(path, data [, binary])`. Broadcast to all connections on path, returns count sent
- `ws.connections(path)`. Count active connections on path
- `ws.connect(url, handlers [, opts])`. Connect to remote WebSocket server (see below)

**Client WebSocket (`ws.connect`):**

```lua
local client = ws.connect("ws://other:8080/feed", {
    on_open = function(conn) conn:send("hello") end,
    on_message = function(conn, msg) log.info(msg) end,
    on_close = function(conn, code, reason) end,
    on_error = function(conn, err) log.error(err) end,
})
```

```javascript
const client = ws.connect("ws://other:8080/feed", {
    onOpen(conn) { conn.send("hello"); },
    onMessage(conn, msg) { log.info(msg); },
    onClose(conn, code, reason) {},
    onError(conn, err) { log.error(err); },
});
```

- Client conn has same `send`/`sendBinary`/`close`/`ping` methods as server conn
- Host allowlist enforced (same as `http.fetch`. Must be in manifest `hosts`)
- Requires running server (`hull` with `-p` port)
- Callbacks fire on the event loop thread (same as server WS callbacks)

### SSE Endpoints

Register Server-Sent Events endpoints with `app.sse(path, handler)`. The handler receives a request object and a stream object.

**Lua:**
```lua
app.sse("/sse/events", function(req, stream)
    stream:event("welcome", json.encode({ time = time.datetime() }))
    for i = 1, 5 do
        hull.sleep(1000)
        stream:event("tick", tostring(i), tostring(i))
    end
    stream:close()
end)
```

**JavaScript:**
```javascript
app.sse("/sse/events", async (req, stream) => {
    stream.event("welcome", JSON.stringify({ time: time.datetime() }));
    for (let i = 1; i <= 5; i++) {
        await hull.sleep(1000);
        stream.event("tick", String(i), String(i));
    }
    stream.close();
});
```

**Stream object:**
- `stream:event(name, data [, id])` / `stream.event(name, data, id?)`. Send SSE event. `name` = event type (null/nil to omit), `data` = event data (multiline auto-split), `id` = event ID (optional)
- `stream:comment(text)` / `stream.comment(text)`. Send SSE comment (keep-alive)
- `stream:close()` / `stream.close()`. End the stream

**Implementation:** Uses Keel's `kl_sse_begin` / `kl_sse_event` / `kl_sse_end` over chunked transfer encoding. The handler runs as a coroutine (Lua) or async function (JS) that can yield with `hull.sleep()` between events.

### Streaming Multipart Uploads

Routes can opt into streaming `multipart/form-data` parsing via `opts.multipart` on `app.<verb>(...)`. The handler runs before the body is buffered and pulls bytes out of the socket on demand via an iterator. There is no `req.body` for these routes - `req:multipart()` / `req.multipart()` is the only way to read the body.

```lua
app.post("/upload", function(req, res)
    for part in req:multipart() do
        if part.filename then
            for chunk in part:chunks() do
                -- handle binary chunk (Lua byte string, #chunk = bytes)
            end
        else
            local value = part:read()
        end
    end
    res:json({ ok = true })
end, { multipart = { max_part_size = 64 * 1024 * 1024, max_total_size = 256 * 1024 * 1024, max_parts = 32 } })
```

```javascript
app.post("/upload", async (req, res) => {
    for await (const part of req.multipart()) {
        if (part.filename) {
            for await (const chunk of part.chunks()) {
                // chunk is an ArrayBuffer; binary-safe (.byteLength = bytes)
            }
        } else {
            const buf = await part.read();   // ArrayBuffer
        }
    }
    res.json({ ok: true });
}, { multipart: { maxPartSize: 64 * 1024 * 1024, maxTotalSize: 256 * 1024 * 1024, maxParts: 32 } });
```

**Caps** (all default to `0` = unlimited): `max_part_size` / `maxPartSize`, `max_total_size` / `maxTotalSize`, `max_parts` / `maxParts`, `max_headers_size` / `maxHeadersSize`, `max_input_buffer` / `maxInputBuffer`. Exceeding any cap raises a parser error which the handler can `pcall` / try-catch to write a structured 4xx response; uncaught errors → 500. Works for both single-read and multi-read bodies - Keel v2.2.0's streaming-async dispatch invokes the handler BEFORE feeding leftover body bytes, so the handler is alive when the cap trips in `on_data`. JS accepts both naming conventions; snake_case wins if both appear.

**Part fields:** `name` (always), `filename` (`nil`/`null` for text fields), `content_type` (Lua) / `contentType` (JS).

**Binary safety:** Lua chunks/`read()` return byte-clean Lua strings. JS chunks/`read()` return `ArrayBuffer` (never JS strings - `JS_NewStringLen` would UTF-8-mangle binary input). To decode text fields in JS, use `new TextDecoder().decode(buf)` (BYOP - QuickJS doesn't bundle it; ASCII can use a manual loop).

**Implementation:**
- Route is registered via `kl_server_route_streaming_async` (Keel v2.2.0+) + a per-runtime factory shim (`hl_{lua,js}_multipart_factory` in `runtime/{lua,js}/routes.c`) that wraps Keel's `kl_body_reader_multipart` with the parkable `hl_cap_multipart_factory` wrapper (`src/hull/cap/body.c`). The async variant means Keel invokes the handler BEFORE feeding leftover body bytes; the handler parks on `NEED_DATA` immediately, and `on_data` resumes it for both leftover and subsequent socket reads.
- The iterator drives `kl_multipart_next()` and on `NEED_DATA` parks the handler via `hl_cap_multipart_park`, sets `c->state = KL_CONN_READING_BODY`, and yields (Lua coroutine / pending JS Promise). The body reader's `on_data` callback fires the park and resumes.
- Handlers that respond without iterating (e.g. auth rejection): Keel forces `keep_alive=0` so stranded body bytes don't bleed into a re-used connection's next request.
- Bindings live in `src/hull/runtime/lua/mod_request.c` and `src/hull/runtime/js/mod_request.c`.

**Known limitations:**
- Live connection required - in-process `hull test` dispatch raises on first `NEED_DATA`. End-to-end coverage is in `tests/e2e_multipart.sh` (run via `make e2e-multipart`).
- (JS) A multipart read that must wait for more body and an attached async op (`hull.sleep`, `db.async`, `http.fetch`, ...) cannot share the request's connection: each takes over its state (the op suspends it, the park sets it reading). So an op started while a read is parked throws a `TypeError` (`Promise.all([it.next(), hull.sleep(10)])`), and so does a read that needs more body after the handler awaited such an op (`for await (const p of req.multipart()) { await db.async.exec(...) }` once the parser needs the next socket read). Both used to corrupt the connection: a partial response went out mid-upload, or the upload stalled until the body timeout. Read the body first, then await the ops (Keel's async completion re-arms no read for a resumed handler that waits for more body - a Keel follow-up).
- `Part` is invalidated after the next iter step (parser is forward-only - don't stash parts past their iteration).
- `chunks(n)` accepts a min-bytes hint that's currently advisory (each parser event = one chunk; coalescing is a follow-up).
- Mid-stream connection close leaks the parked continuation - production deployments should run behind a reverse proxy with request timeouts.

See [docs/multipart.md](multipart.md) for the full API + `examples/multipart_upload/` for a runnable Lua + JS demo.

### Static File Serving

Convention-based: place files in `app_dir/static/`, they're served at `/static/*`.

- **Dev mode:** Reads from disk via `kl_response_file()` (zero-copy sendfile). `Cache-Control: no-cache`.
- **Build mode:** Files are embedded in the binary via `hl_app_entries[]` (with `static/` prefix) and looked up via VFS (`hl_vfs_find`). `Cache-Control: public, max-age=86400`.
- **ETag/304:** `W/"<size_hex>"` for embedded, `W/"<mtime_hex>-<size_hex>"` for filesystem. Returns 304 on `If-None-Match`.
- **MIME types:** Extension-based lookup (21 types: html, css, js, json, png, jpg, svg, woff2, etc.). Default: `application/octet-stream`.
- **Security:** Rejects `..` path traversal, null bytes, leading `/` in relative paths.
- **Auto-detection:** Middleware is registered only when `hl_vfs_has_prefix(app_vfs, "static/")` returns true or `static/` directory exists on disk. User routes take priority (registered first).

Implementation: `src/hull/static.c` + `include/hull/static.h`. Uses `HlVfs` for O(log n) embedded lookup. Registered as a Keel pre-body middleware via `kl_server_use()`.

Embedding paths:
- `make APP_DIR=myapp`. Makefile discovers all app files, generates sorted `app_registry.c` with single `hl_app_entries[]`
- `hull build myapp`. `build.lua` discovers all files, generates sorted `app_registry.c`
- All file types share one `HlEntry` array, sorted by name (`LC_ALL=C sort`), disambiguated by naming convention: `./` (modules), `templates/`, `static/`, `migrations/`
- At runtime, consumers use `HlVfs` for O(log n) lookups instead of O(n) linear scans

### Recommended Middleware Stack

Order matters. Each middleware runs before the next:

```lua
local cors        = require("hull.web.middleware.cors")
local ratelimit   = require("hull.web.middleware.ratelimit")
local auth        = require("hull.web.middleware.auth")
local csrf        = require("hull.web.middleware.csrf")
local session     = require("hull.web.middleware.session")
local logger      = require("hull.web.middleware.logger")
local health      = require("hull.web.middleware.health")
local etag        = require("hull.web.middleware.etag")
local transaction = require("hull.web.middleware.transaction")
local idempotency = require("hull.web.middleware.idempotency")

session.init()       -- create hull_sessions table
idempotency.init()   -- create _hull_idempotency_keys table

-- Pre-body middleware (runs before body is read)
-- 0. Health checks. /health (liveness) and /ready (readiness)
app.use("GET", "/*", health.middleware())
-- 1. Logging. Assign request ID, log method + path
app.use("*", "/*", logger.middleware({ skip = {"/health"} }))
-- 2. Rate limiting. Reject abusive traffic before doing any work
app.use("*", "/api/*", ratelimit.middleware({ limit = 60, window = 60 }))
-- 3. CORS. Must run before auth so preflight doesn't require credentials
app.use("*", "/api/*", cors.middleware({ origins = { "https://myapp.com" } }))
-- 4. Authentication. Session or JWT
app.use("*", "/api/*", auth.session_middleware({}))

-- Post-body middleware (runs after body is read)
-- 5. CSRF. Needs body for form token (session-based apps only, not JWT)
app.use_post("*", "/*", csrf.middleware({ secret = "change-me" }))
-- 6. Transaction. Wrap mutations in BEGIN IMMEDIATE..COMMIT
app.use_post("POST", "/api/*", transaction.middleware())
-- 7. Idempotency. Cache POST responses by Idempotency-Key header
app.use_post("POST", "/api/*", idempotency.middleware())
-- 8. Route handlers. Use etag.json() instead of res:json() for ETag support
app.get("/api/items", function(req, res)
    local items = db.query("SELECT * FROM items")
    etag.json(req, res, { items = items })
end)
```

### Best Practices

- **Middleware order matters:** Rate limit before auth (reject early, save work). CORS before auth (preflight must not require credentials).
- **Scope middleware to paths:** Use `"/api/*"` not `"/*"` for CORS and rate limiting. Public routes (health checks, static assets) shouldn't be rate limited or require auth.
- **Use `req.ctx` for data passing:** Middleware stores data in `req.ctx` (e.g. `session_id`, `user`, `csrf_token`) for downstream handlers.
- **CORS origins:** Always list explicit origins in production. Never use `"*"` with `credentials = true`.
- **Rate limiting keys:** Default `"global"` key rate-limits all clients together. Use a key function for per-user limits: `key = function(req) return req.ctx.user_id or req.headers["x-forwarded-for"] or "anon" end`.
- **CSRF is for cookies only:** Session/cookie auth needs CSRF protection. JWT Bearer auth does not (tokens aren't sent automatically by browsers).
- **Session init at startup:** Call `session.init()` before registering routes. It creates the SQLite table.
- **Lua vs JS differences:** The Lua and JS APIs are functionally equivalent but differ in naming conventions (snake_case vs camelCase) and some defaults. See the JS stdlib source for JS-specific option names.

### Outbound SMTP (`smtp.send`, model-2 async)

`smtp.send(msg)` delivers mail over the Keel-v3 transport. Under an active server
event loop it runs the **model-2** path: the SMTP conversation executes on a bounded
worker thread while the request coroutine/Promise suspends, so a slow peer never
blocks the loop. Full design: [docs/smtp_keel_slice2c_plan.md](smtp_keel_slice2c_plan.md).

- **Lua:** `local r = smtp.send(msg)` yields transparently and resumes with the
  `{ ok, error }` table. **JS:** `const r = await smtp.send(msg)` - it ALWAYS returns
  a `Promise` resolving to `{ ok, error }` (including immediately-resolved
  validation/admission failures); it rejects only on a programming error (bad
  argument type), never on an SMTP failure. There is no `smtp.async.send`; `send`
  IS the async entry. The stdlib `hull/email` provider `await`s it (see
  `stdlib/js/hull/email.js`).
- **No active loop** (`app.main` CLI, in-process test harness): the same call runs
  synchronously on the calling thread (Lua returns the table; JS returns an
  already-resolved Promise). This is the ONLY synchronous path; the runtime never
  falls back to synchronous execution from an active loop.
- **Admission cap.** Concurrent SMTP worker jobs are capped at `max(1, floor(W/2))`
  for a pool of `W` workers (`--workers`), always leaving >= 1 worker for db /
  compute. When the cap is reached, `send` resolves promptly (never runs on the loop
  thread) with `{ ok = false, error = "connect_failed" }` and audits
  `schedule:cap_reached`.
- **Terminal audit tags** (with `--audit`): a post-resolution operation-deadline
  expiry keeps the public `connect_failed` token but audits
  `terminal:post_resolution_deadline`; a shutdown-swept in-flight op audits
  `terminal:cancelled`; a transport that could not confirm detachment audits
  `teardown:leaked`. Exactly one completion record per send.
- **Host allowlist.** `msg.host` must be in `manifest.hosts` (same matcher as
  `http.fetch` / `ws.connect`); a denied host is audited `result:denied` before any
  submit. **STARTTLS/implicit TLS** verify the chain + hostname against the embedded
  CA bundle and fail closed (no plaintext fallback).

### Background Timers

`app.every()` and `app.daily()` register repeating timer callbacks that run on the event loop thread. Timer callbacks support the full async runtime (`hull.sleep()`, `http.fetch()`, `db.*`).

**Lua:**
```lua
-- Repeating interval (milliseconds, minimum 100ms)
app.every(5000, function()
    session.cleanup()
end)

-- Async operations work inside timers
app.every(30000, function()
    outbox.flush()  -- makes HTTP requests
end)

-- Return false to self-cancel
app.every(1000, function()
    local pending = outbox.flush()
    if pending == 0 then return false end
end)

-- Daily at wall-clock time (UTC by default)
app.daily("02:00", function()
    outbox.cleanup(86400 * 30)
end)

-- Daily at local time
app.daily("02:00", function()
    inbox.cleanup()
end, { localtime = true })
```

**JavaScript:**
```javascript
app.every(5000, () => { session.cleanup(); });

app.every(30000, async () => { await outbox.flush(); });

app.every(1000, () => {
    const pending = outbox.flush();
    if (pending === 0) return false;  // self-cancel
});

app.daily("02:00", () => { outbox.cleanup(86400 * 30); });
app.daily("02:00", () => { inbox.cleanup(); }, { localtime: true });
```

**Constraints:**
- Minimum interval: 100ms (enforced, prevents tight loops)
- No `req`/`res`. These are background tasks, not request handlers
- Errors are logged but don't stop the timer (re-schedules regardless)
- One invocation at a time. If a callback is still running (async yield), the next tick is deferred
- Return `false` to stop the repeating timer
- `app.daily("HH:MM")` defaults to UTC. Pass `{ localtime = true }` for local time

**Implementation:** Timer callbacks fire via Keel's `kl_timer_add` min-heap. Self-re-adding callbacks give repeating behavior. Async operations use "detached" mode. `HlAsyncCtx` with `detached=1` resumes via `hl_async_ctx_resume_detached()` instead of `kl_async_complete()`.

### Concurrent work (`hull.async`, `hull.gather`, `hull.map`)

Everything a Hull program does runs on one event loop, and a coroutine that
waits (network, a worker-pool job, `hull.sleep`) lets the others run. These
start work concurrently and wait for it - most usefully from `app.main`,
which is otherwise one coroutine doing things in turn.

**Lua:**
```lua
local t = hull.async(function(url) return fetch(url) end, url)  -- a task
local body = t:wait()          -- its return values, or its error raised
t:done()                       -- finished yet?

local a, b = hull.gather(      -- a fixed set; first results, in order
    function() return fetch(x) end,
    function() return fetch(y) end)

local up = hull.map(hosts, function(host, i)   -- a list, at most `limit` at once
    local c = assert(ssh.connect{ host = host, user = "deploy", key = key })
    local r = c:exec("uptime")
    c:close()
    return r.stdout
end, { limit = 8 })            -- default 16; math.huge for no cap
```

**JS:** Promises already give tasks; for a gather that waits for every one use
`Promise.allSettled` (`Promise.all` rejects at the first failure and leaves the
rest running). The bounded fan-out is
`await hull.map(items, async (item, i) => ..., { limit: 8 })` (default 16,
`Infinity` for no cap).

- **Failures:** `gather` and `map` let every item finish, then raise the
  first failure (by position) with all of them attached: `err.errors[i]`
  (JS: `e.errors[i]`, sparse). `tostring(err)` / `e.message` is the first
  one's message. There is no fail-fast: nothing in flight can be abandoned
  halfway, and its own timeout already bounds it. A Lua function that raised
  no value (`error()`) appears in `errors` as `"(error with no value)"`.
- **Items (Lua):** `map` takes `items[1..items.n]` when the list has an `n`
  (as `table.pack` makes), else `items[1..#items]`. `results` is a plain
  list (it encodes as JSON); where `fn` returned nil it has a hole.
- **Where you can wait:** `task:wait()`, `gather`, `map` and `hull.sleep` wait
  only in a handler, a task, or `app.main` - not while a module loads, inside a
  C callback such as `string.gsub` or `table.sort`, or in a coroutine the app
  created itself. There they raise at once.
- **Concurrency, not parallelism:** tasks overlap while they wait; CPU-bound
  Lua does not get faster. `compute.async` / `db.async` fan-out does use the
  worker pool's threads.
- **One connection per task:** an SSH connection (or anything else that parks
  per coroutine) is used by the task that opened it; from another task while
  one waits it is `busy`.
- **Budgets:** each task starts with its own instruction limit, like a timer
  callback, counted over the task's whole run (waiting does not top it up).
- **`app.main` returning with tasks unjoined** ends the process and abandons
  them, with a WARN saying how many - join what you start. In an app that
  serves routes, `app.main` returning 0 does not end the process, so its tasks
  keep running.

`tui.async(fn)` stays fire-and-forget. Design:
[task_join_design.md](task_join_design.md).

### WASM Compute Plugins

Hull supports compute-only WASM plugins for CPU-intensive pure functions. Plugins have no I/O. They transform input bytes to output bytes inside isolated WASM linear memory with gas-metered execution.

**Directory convention:** Place `.wasm` files in `app_dir/compute/`. Module name = filename without extension (e.g. `compute/score.wasm` → `"score"`).

**Lua API:**
```lua
compute.available()                     -- boolean (WASM runtime initialized?)

-- Synchronous call (gas-limited, blocking)
local output, err = compute.call("score", input_bytes, {
    max_input  = 64 * 1024,    -- 64 KB (optional, has defaults)
    max_output = 64 * 1024,    -- 64 KB
    gas        = 10000000,     -- 10M instructions (interpreted code only)
    timeout_ms = 2000,         -- wall-clock bound, AOT included (default 10 s)
    heap       = 256 * 1024,   -- 256 KB WASM heap
})
if err then
    -- err: "not_found", "gas_exhausted", "timeout", "output_too_small",
    --       "input_too_large", "call_failed", "segments_in_use",
    --       "too_many_instances", "internal_error"
end

-- Async call (yields to event loop, request handler only)
-- Dispatches WASM execution to thread pool. Other requests served while running.
local r = compute.async.call("score", input_bytes, opts)
-- r.result = output string on success, r.error = error string on failure

-- Preload module into cache
compute.load("score")

-- Create a WasmBuffer from a string (for zero-copy chaining)
local buf = compute.buffer("input data")
```

**JavaScript API:**
```javascript
import { compute } from "hull:compute";

compute.available()                    // boolean

// Sync: Input: string or ArrayBuffer. Output: ArrayBuffer.
const output = compute.call("score", inputBytes, {
    maxInput: 64 * 1024,
    maxOutput: 64 * 1024,
    gas: 10000000,
    timeoutMs: 2000,
});

// Async: returns Promise. Dispatches to thread pool.
const buf = await compute.async.call("score", inputBytes, opts);

compute.load("score");

// Create a WasmBuffer from a string (for zero-copy chaining)
const buf = compute.buffer("input data");
```

**Gas vs timeout - two different bounds.** `gas` is WAMR instruction
metering: exact, but it applies to the **interpreter only** - WAMR never
meters AOT code, and AOT is what `hull build` embeds when `wamrc` is present.
`timeout_ms` (`timeoutMs` in JS) is a **wall-clock** bound on the whole call,
instantiation included: a watchdog thread terminates the instance at the
deadline (`wasm_runtime_terminate`; WAMR patch 0007 makes interpreted code
poll for it every 4096 instructions and AOT code at every loop header), and
the call fails with `"timeout"`. It is the bound that holds for **every**
guest execution - AOT code, interpreted code, and the module's own start /
ctor functions, which run inside instantiation where gas never applied
(a module whose start function outlives the timeout at load is refused).
Default 10 s, maximum 1 h. Both are ceilings configured the same way: per call
(`gas`, `timeout_ms`), in the manifest (`wasm = { gas = N, timeout_ms = N }`,
JS `wasm: { gas, timeoutMs }`), and by the operator (`--wasm-gas N`,
`--wasm-timeout-ms N`); a per-call value may lower the ceiling, never raise
it. The ceilings apply on every entry point - the server, an `app.main`
program, `hull test` and `hull agent` - and to `compute.stream` and WASM
`db.udf` instances too. `compute.instance` takes `timeout_ms` / `gas` as its
per-call defaults: a call on the instance that sets neither uses them (the
ceiling still caps both). An AOT artifact without the stamp of Hull's patched
`wamrc` (an upstream / distro `wamrc`, or one older than patch 0007) has no
loop-header terminate check, so the runtime refuses it and runs the module's
`.wasm` in the interpreter instead (or fails to load it when there is no
`.wasm`), and `hull build` does not embed one - rebuild it with Hull's
`wamrc` (`hull tools install wamrc`).

**Sync vs Async:** Use `compute.call()` for fast/small computations (sub-ms) and in tests/timers. Use `compute.async.call()` in request handlers for expensive computations. It yields to the event loop so other requests are served concurrently. The async variant follows the same pattern as `db.async.query()`.

**Shared data segments**. `compute.segment()` loads named read-only data segments that all instances of a module can read at native speed via WAMR shared heaps:

```lua
-- Lua: load named segments for a module
compute.segment("routing", "graph", graph_bytes)       -- segment 0
compute.segment("routing", "landmarks", fs.mmap("landmarks.bin"))  -- zero-copy
compute.segment("routing", "grid", nil)                -- remove segment
compute.segment("routing", nil)                        -- remove all segments
-- Use normally. Segments auto-attached to every instance
local out = compute.call("routing", query)
```

```javascript
// JS
compute.segment("routing", "graph", graphBytes);
compute.segment("routing", null);                      // remove all
```

WASM plugins query segments via `host_call(0x02, segment_id, sub)`:
- `host_call(0x02, seg_id, 0)` → WASM address of segment (0 if not loaded)
- `host_call(0x02, seg_id, 1)` → size of segment
- `host_call(0x02, -1, 0)` → total segment count

Segments are page-aligned mmap regions in the high end of WASM32 address space. Up to 16 segments per module, 3 GB total. Adding/removing segments drains the instance pool. A segment change is refused with `"segments_in_use"` while an instance outside the pool still holds the module's segments - a live `compute.instance`, or a zero-copy `WasmBuffer` result not yet closed / collected - so close those first (load segments at startup, before dispatching work). An instance that was out of the pool across a change is destroyed when released, never pooled with the old segments. At most 128 instances may hold one module's segments at once (`"too_many_instances"`): WAMR counts them in 8 bits.

**Mapped spans (per-invocation, zero-copy host-backed file windows).** Where `compute.segment` is module-scoped shared data, a **mapped span** attaches a host-`mmap`'d file window read-only to **ONE** `compute.call` and detaches on every exit path. It lets a WASM plugin scan/parse a very large file (OSM PBF, Parquet, raster, model blob) with **zero copy into linear memory** and kernel demand paging, reading it in place through ordinary bounds-checked loads with **no per-access host call**. Design: [docs/wasm_mapped_spans_design.md](wasm_mapped_spans_design.md); WAMR patch: [docs/wamr_patches.md](wamr_patches.md); worked example: `examples/mapped_spans/`.

Host side (Lua; JS is the camelCase parallel):
```lua
local fs, compute = require("hull.fs"), require("hull.compute")
-- fs.mmap(path) maps the whole file; {offset,length} maps a WINDOW (page
-- alignment handled internally, non-page-aligned offsets OK).
local w = fs.mmap("data.bin", { offset = 8195, length = 4096 })
local out, err = compute.call("spanreader", input, {
    spans = { { name = "source", buffer = w } },   -- attach RO for THIS call only
})
w:close()   -- refcounted borrow; safe to close while a borrower (image/gpu) lives
```

Guest side - the shipped SDK header `hull/wasm/span.h` (canonical
`templates/hull_span.h`, byte-synced to every example/fixture copy via
`tests/check_sdk_headers.sh`). The **same** source compiles natively for
differential tests + the benchmark:
```c
#include "hull_compute.h"
#include "hull_span.h"
HullSpan spans[HULL_SPAN_MAX];
int n = hull_span_setup(spans, HULL_SPAN_MAX);      // SPAN_INFO setup only: one count query + one query per span
int i = hull_span_find(spans, n, "source");
const void *w = (const void *)(hull_span_uptr)spans[i].base;
uint32_t v; hull_span_read_u32le(w, spans[i].len, off, &v);   // bounds-checked, inlineable
```
Typed accessors are read-only and cover `u8/i8`, `u16/u32/u64` + signed + `f32/f64`, each in LE and BE (`hull_span_read_*`); every read is overflow-safe (`hull_span__fits`) and returns `HULL_SPAN_ERR_RANGE` rather than reading OOB. **Store accessors / writable spans are a deliberately-deferred follow-up** - the initial cut is read-only.

Guarantees (all tested; see `tests/hull/cap/test_wasm_spans.c`, `tests/hull/test_span_sdk.c`, the `fuzz_span_sdk`/`fuzz_span_window` harnesses, and `tests/hull/cap/test_fs.c::mmap_window_demand_paging`):
- **No raw native pointer** reaches WASM: `spans[i].base` is a guest-domain address the WAMR guarded-subrange shared heap translates + bounds-checks; under AOT the read lowers to a cached-range check + a native load, no host trap per access.
- **Strict per-instance isolation**: a module can only address spans explicitly passed to its own invocation; it cannot manufacture an address reaching Hull memory, another module's private/mapped memory, or unrelated mappings. Cross-instance isolation is CI-gated.
- **Read-only at metadata AND OS page level**; a guest write to the mapped region traps recoverably (call fails), never crashes the host.
- **wasm32 windowing**: a `HullSpan` carries a 64-bit logical `foffset`, so a parser moves a window (≤ 1 GiB per window, `HL_FS_MMAP_MAX_WINDOW_BYTES`) through a file far larger than the 4 GiB WASM space without copying. **Memory64** allows whole-file spans (validated: `memory64_span_readback`).
- **Lifetime**: the `MappedBuffer` owns the mapping; a span borrows it. `buf:close()` while a borrower is alive defers the `munmap` (refcounted); a span never outlives its mapping (use-after-unmap is impossible). The detach-before-destroy ordering holds on all instance paths.

**Performance (measured, honest).** The **proven** properties are zero-copy and no-per-access-host-call. Throughput is **workload- and codegen-dependent, NOT universally near-native** - faster than the checked native baseline for wide/multi-pass reads, slower for record parsing, vectorization-sensitive for byte scans; shared-runner timings are informational (cross-run variance too high to gate). See the four-workload × four-impl benchmark ([docs/mapped_span_benchmark_design.md](mapped_span_benchmark_design.md), `make bench-mapped-span`) for the recorded numbers and the corrected claim.

**Plugin ABI:** Plugins must export `hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written` and optionally `hull_version() -> int`. Single import: `env.host_call(opcode, ptr, len) -> int` (LOG=0x01, DATA_INFO=0x02, SPAN_INFO=0x04, CALLBACK=0x10).

**Build & AOT:** `hull build` embeds `compute/*.wasm` files and auto-compiles them to AOT if `wamrc` is available. AOT modules are embedded alongside `.wasm` files; at runtime, AOT is preferred over interpreter.

```bash
make wamrc                         # build AOT compiler (one-time, requires cmake + LLVM)
hull build myapp                   # auto-AOT compiles compute/*.wasm during build
hull build myapp --no-aot          # skip AOT compilation
hull build myapp --target=x86_64   # cross-compile AOT for different arch
```

For cosmocc builds, both x86_64 and aarch64 AOT files are generated automatically.

**Dev mode AOT:** In `hull dev`, place pre-compiled `.aot.<arch>` files next to `.wasm` files in `compute/` and they'll be loaded automatically (four-tier lookup: VFS AOT → VFS WASM → filesystem AOT → filesystem WASM).

**wamrc build:** `make wamrc` builds the WAMR AOT compiler from `vendor/wamr/wamr-compiler`. Requires cmake and LLVM (`brew install llvm` on macOS, `apt install llvm` on Linux). Override LLVM path: `make wamrc WAMRC_CMAKE_FLAGS="-DLLVM_DIR=/path/to/llvm/cmake"`. Output: `build/wamrc`.

**Configuration:** Controlled by `HL_ENABLE_WASM` (default: 1). Disable with `make HL_ENABLE_WASM=0`. WAMR adds ~256 KB to the binary.

**SIMD128:** Enabled (`-DWASM_ENABLE_SIMD=1`). Compile plugins with `-msimd128` (C) or `#[target_feature(enable = "simd128")]` (Rust). AOT maps to native SSE4.1/NEON. Interpreter cannot load v128 modules (graceful error).

**Instance pooling:** Reuses WASM instances across `compute.call()` invocations (pool max 8 per module, heap ≤ 4 MB). Reduces per-call overhead from ~2.5ms to near-zero.

**Persistent instances:** `compute.instance(name, opts?)` creates a long-lived WASM instance that retains linear memory across calls. Not pooled. Exclusively owned until `close()` or GC. Supports sync (`inst:call`/`inst.call`), async (`inst.async:call`/`inst.async.call`), and buffer mode. Gas resets per call; heap/stack are immutable. Use for stateful workloads (ML weights, pre-built indexes) where per-call instantiation cost is too high.

**Memory limits:** Configurable at three tiers. Per-call opts, CLI flags (`--wasm-heap 512M`), and compile-time (`make HL_WASM_MAX_HEAP_MB=512`). Default: 2 MB heap, 1 MB I/O. Max: ~4 GB heap, 256 MB I/O (WASM32) / 16 GB I/O (Memory64).

**Memory64:** modules compiled with 64-bit memory (`(memory i64 N)`) are detected automatically; Memory64 **requires AOT** (the fast interpreter does not support it) and `wamrc` auto-detects it from the module's `(memory i64)` type (there is no `--enable-memory64` wamrc flag in the vendored WAMR); the `hull_process` ABI becomes `(i64, i64, i64, i64) -> i32`, dispatched (8 argv cells) by the module's `is_memory64` flag. Detection goes through the public WAMR accessor `wasm_runtime_memory_is_memory64` (patch 0005), so `cap/wasm.c` needs no WAMR-internal header and no WAMR compile-time config - the config-dependent `WASMMemoryInstance` layout stays inside WAMR. Detection, the `memory64_requires_aot` guard, and the 8-cell AOT dispatch/readback are shipped and CI-gated (the `memory64_aot_dispatch` must-not-skip leg runs a real `echo64` AOT via `hl_cap_wasm_load`/`call`). See [docs/memory64_dispatch_design.md](memory64_dispatch_design.md) (#318). **Mapped spans under Memory64 are also validated** ([#334](https://github.com/artalis-io/hull/issues/334), [docs/memory64_spans_design.md](memory64_spans_design.md)): a `(memory i64)` guest reads a span window placed above `UINT32_MAX` through the SPAN_INFO record's 64-bit `base`, CI-gated (`memory64_span_readback`, must-not-skip). **`hull build` of a Memory64 compute plugin is also supported** ([#336](https://github.com/artalis-io/hull/issues/336), [docs/memory64_build_design.md](memory64_build_design.md), CI-gated `tests/e2e_compute_memory64.sh` on x86_64 + aarch64): the production path AOT-compiles a `compute/*.wasm` mem64 module (wamrc auto-detects it - no `--enable-memory64` flag) and keeps the transparent `--enable-shared-heap`, which is safe even for a heap-less call. (The earlier "segfaults heap-less" worry was a 32-bit-*target*-only WAMR bug; on Hull's x86_64/aarch64 AOT targets WAMR's `aot_runtime.c:2134` sets the safe `UINT64_MAX` sentinel, confirmed by an isolation experiment - see the design record.)

**Streaming I/O:**
```lua
-- Buffer → buffer
local result = compute.stream("module", input_data, nil, { chunk_size = 65536 })

-- File → file (never fully in memory)
compute.stream("module", { file = "input.csv" }, { file = "output.json" }, { chunk_size = 65536 })

-- Buffer → callback
compute.stream("module", data, function(chunk, index, is_last) end, { chunk_size = 65536 })
```

- Input: string, WasmBuffer, MappedBuffer, or `{ file = "path" }`
- Output: nil (return buffer), `{ file = "path" }`, or callback function
- Uses persistent instance internally. State preserved between chunks
- Modules can query chunk metadata via `host_call(0x03)`: `hull_stream_is_first()`, `hull_stream_is_last()`, `hull_stream_chunk_index()`

**Architecture:** See `docs/wamr_architecture.md` for the full design document.

### GPU Compute (wgpu-native)

Hull supports GPU compute shaders via wgpu-native v27 (Vulkan/Metal/DX12). Disabled by default. Enable with `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu`.

**Manifest declaration:** Apps must declare `gpu: true` in their manifest to access the `gpu` global. Apps without a manifest get GPU access by default (backward compat).

```lua
app.manifest({ gpu = true })
```

**Lua API:**
```lua
gpu.available()                        -- boolean
gpu.devices()                          -- { {id=0, name="Apple M1"}, ... }
gpu.compile(name, wgsl)                -- compile WGSL shader (cached, idempotent)
gpu.load(name)                         -- load + compile shaders/<name>.wgsl from disk/VFS
gpu.dispatch(name, opts)               -- run shader, return output (string)
gpu.dispatch(name, { output = false }) -- fire-and-forget (no readback, returns true)
gpu.pipeline(stages, opts)             -- multi-stage dispatch, single submission
gpu.pipeline(stages, { output = false }) -- fire-and-forget pipeline
gpu.async.dispatch(name, opts)         -- async dispatch (yields to event loop)
gpu.async.pipeline(stages, opts)       -- async pipeline
gpu.buffer(name, data)                 -- create/write persistent buffer (string or MappedBuffer)
gpu.buffer(name, nil)                  -- destroy buffer
gpu.buffer_read(name)                  -- read buffer back to host
gpu.buffer_copy(src, dst, opts?)       -- GPU-side buffer copy (no CPU roundtrip)
```

**JavaScript API:**
```javascript
import { gpu } from "hull:gpu";
gpu.available()                        // boolean
gpu.devices()                          // [{id, name}, ...]
gpu.compile(name, wgsl)                // compile WGSL shader
gpu.load(name)                         // load + compile shaders/<name>.wgsl
gpu.dispatch(name, opts)               // ArrayBuffer output
gpu.dispatch(name, { output: false })  // fire-and-forget (returns true)
gpu.pipeline(stages, opts)             // multi-stage, ArrayBuffer or Array<ArrayBuffer>
gpu.pipeline(stages, { output: false })// fire-and-forget pipeline
gpu.async.dispatch(name, opts)         // Promise<ArrayBuffer>
gpu.async.pipeline(stages, opts)       // Promise
gpu.buffer(name, data)                 // create/write (ArrayBuffer, MappedBuffer, or string)
gpu.buffer(name, null)                 // destroy
gpu.bufferRead(name)                   // ArrayBuffer
gpu.bufferCopy(src, dst, opts?)        // GPU-side buffer copy
```

**Dispatch options:**
```lua
gpu.dispatch("shader_name", {
    uniforms = packed_binary,          -- binding 0 (16-byte aligned)
    buffers = {
        { data = bytes, usage = "read" },       -- binding 1
        { name = "persistent", usage = "read" }, -- binding 2 (named buffer)
        { size = N, usage = "readwrite" },       -- binding 3 (output)
    },
    textures = {                               -- optional texture bindings
        { name = "input" },                    -- sampled (paired: texture + sampler)
        { name = "output", storage = true },   -- storage texture (single binding)
    },
    workgroups = { x = 64, y = 1, z = 1 },
    output = 3,                        -- 1-indexed buffer to read back (Lua)
    output_texture = 2,                -- 1-indexed texture to read back as HlImage (Lua)
    device = -1,                       -- -1 = default device
})
```

**Pipeline (multi-stage dispatch):**
```lua
-- Single command buffer submission, single poll, single readback.
-- Named buffers are shared across stages (max declared size allocated).
local out = gpu.pipeline({
    { shader = "normalize", buffers = {{ name = "data", data = input }}, workgroups = {x=64} },
    { shader = "score",     buffers = {{ name = "data" }, { name = "results", size = N*4 }},
                            uniforms = params, workgroups = {x=64} },
    { shader = "top_k",     buffers = {{ name = "results" }}, workgroups = {x=1} },
}, {
    outputs = { { stage = 3, buffer = 1 } },  -- 1-indexed (Lua)
    device = -1,
})
-- Single output: returns string. Multiple outputs: returns table of strings.
```
```javascript
// JS: 0-indexed outputs
const out = gpu.pipeline([
    { shader: "normalize", buffers: [{ name: "data", data: buf }], workgroups: {x:64} },
    { shader: "score",     buffers: [{ name: "data" }, { name: "results", size: N*4 }],
                           uniforms: paramsBuf, workgroups: {x:64} },
    { shader: "top_k",     buffers: [{ name: "results" }], workgroups: {x:1} },
], { outputs: [{ stage: 2, buffer: 0 }] });
// Single output: ArrayBuffer. Multiple outputs: Array<ArrayBuffer>.
```

**Fire-and-forget dispatch:** Set `output = false` to skip readback. The shader executes and persistent buffers are updated in-place, but no data is returned to the host. Returns `true` on success. Works with both `gpu.dispatch()` and `gpu.pipeline()`.

```lua
-- Update embeddings in-place on GPU (no readback)
gpu.dispatch("normalize", {
    buffers = {{ name = "embeddings" }},
    workgroups = { x = 1024 },
    output = false,
})
-- Pipeline fire-and-forget: double → triple in-place
gpu.pipeline({
    { shader = "double", buffers = {{ name = "data" }}, workgroups = {x=64} },
    { shader = "triple", buffers = {{ name = "data" }}, workgroups = {x=64} },
}, { output = false })
```

**GPU-side buffer copy:** Copy between persistent GPU buffers without CPU roundtrip.

```lua
gpu.buffer_copy("source", "dest")                           -- full copy
gpu.buffer_copy("source", "dest", { size = 1024 })          -- partial
gpu.buffer_copy("source", "dest", {
    src_offset = 0, dst_offset = 512, size = 256,           -- with offsets
})
```

**GPU Textures:**
- `gpu.texture(name, img)`. Create persistent texture from HlImage.
- `gpu.texture(name, data, opts)`. Create from raw bytes with `opts.width`, `opts.height`, `opts.format`, `opts.storage`.
- `gpu.texture(name, nil)`. Destroy persistent texture.
- `gpu.texture_read(name)` → HlImage. Read back texture pixels.
- Dispatch with textures:
  ```lua
  gpu.dispatch("shader", {
      textures = {
          { name = "input" },                        -- sampled (binding N, N+1)
          { name = "output", storage = true },       -- storage (binding M)
      },
      output_texture = 2,                            -- readback as HlImage
  })
  ```
- Sampled textures get paired bindings (texture view + sampler). Storage textures get single binding.
- Binding convention: uniforms → buffers → sampled textures → storage textures (all `@group(0)`).

**Shader loading from files:** `gpu.load(name)` reads `shaders/<name>.wgsl` from disk (dev mode) or VFS (built binaries) and compiles it. Enables shader iteration without modifying app code.

```lua
-- shaders/score.wgsl on disk (dev mode) or embedded (built binary)
gpu.load("score")                     -- reads + compiles shaders/score.wgsl
-- equivalent to: gpu.compile("score", <file contents>)
```

**Shader embedding in builds:** `hull build` and `make APP_DIR=` automatically discover and embed `shaders/*.wgsl` files into the binary via the VFS, just like `templates/`, `static/`, `compute/`, and `migrations/`. `gpu.load()` checks VFS first, then falls back to disk. So shaders work identically in dev mode and built binaries.

**Directory convention:**
```
myapp/
  app.lua
  shaders/             ← WGSL compute shaders (gpu.load)
    normalize.wgsl
    score.wgsl
  compute/             ← WASM plugins (compute.call)
    echo.wasm
  templates/           ← HTML templates
  static/              ← Static assets
  migrations/          ← SQL migrations
```

**Buffer sharing in pipelines:** Named buffers are created once and reused across stages. When multiple stages reference the same buffer name with different sizes, the maximum declared size is allocated. First stage with `data` uploads initial content; subsequent stages reuse the existing buffer. Persistent buffers (created via `gpu.buffer()`) participate by name.

**Memory-mapped file input (fs.mmap → GPU):**
```lua
-- Zero-copy: disk → mmap → GPU buffer (no Lua string intermediary)
app.manifest({ gpu = true, fs = { read = {"embeddings.bin"} } })

local mapped = fs.mmap("embeddings.bin")  -- mmap'd pointer
gpu.buffer("vectors", mapped)              -- pointer → wgpuQueueWriteBuffer directly
mapped:close()

-- Also works inline in dispatch/pipeline buffers:
local out = gpu.dispatch("search", {
    buffers = {{ data = mapped, size = mapped:len() }},
    workgroups = { x = 1024 },
    output = 1,
})
```

`gpu.buffer()`, `gpu.dispatch()`, and `gpu.pipeline()` all accept `MappedBuffer` (from `fs.mmap()`) as buffer data in both Lua and JS. This avoids copying large datasets through the scripting runtime.

**Binding layout:** Uniforms at binding 0 (if present), storage buffers at binding 1..N. WGSL shader `@binding()` annotations must match this auto-layout.

**Async dispatch:** `gpu.async.dispatch()` and `gpu.async.pipeline()` submit GPU work to the thread pool and yield to the event loop (Lua coroutine / JS Promise). Other requests are served while the GPU is working. Deep-copies all buffer data for thread safety.

**Sandbox:** When `manifest.gpu` is set:
- macOS: allows `iokit-open` and `com.apple.MTLCompilerService` mach-lookup
- Linux: unveils `/dev/dri` (rw) and `/proc/self` (r)

**Performance (Apple M1 Max, cosine similarity on 128-dim vectors):**

| Vectors | Native C | WASM AOT | GPU | GPU vs AOT |
|---------|----------|----------|-----|------------|
| 64 | 7 µs | 7 µs | 2,630 µs | 0.0x |
| 1K | 118 µs | 108 µs | 2,630 µs | 0.0x |
| 16K | 1,830 µs | 2,534 µs | 2,629 µs | 1.0x |
| 64K | 7,270 µs | 10,969 µs | 2,653 µs | **4.1x** |

GPU latency is constant ~2.6ms (dominated by submit+poll overhead). Crossover vs AOT at ~16K vectors. Use GPU for large parallel workloads; use WASM AOT for small sequential ones.

**GPU timeout:** Dispatches time out after 5 seconds (configurable via `HL_GPU_TIMEOUT_MS` at compile time). Returns `HL_GPU_ERR_TIMEOUT`. Prevents infinite hangs from shader bugs. Applies to `dispatch`, `pipeline`, and `buffer_copy`.

**Build:** `make fetch-wgpu` downloads and SHA-256 verifies wgpu-native. Then `make HL_ENABLE_GPU=1` auto-detects `vendor/wgpu/`. macOS links Metal + QuartzCore + CoreGraphics + Foundation; Linux links `-lvulkan`. Not compatible with Cosmopolitan builds.

### Unified Buffer Protocol

All compute and GPU functions accept any buffer type as input via the unified buffer protocol (`HlBufferView` in `include/hull/buffer.h`):

| Type | Source | Lua | JS |
|------|--------|-----|-----|
| String | Literals, `string.pack` | Default | N/A |
| ArrayBuffer | JS typed arrays | N/A | Default |
| MappedBuffer | `fs.mmap(path)` | Userdata | Object |
| WasmBuffer | `compute.call(name, input, { buffer = true })` | Userdata | Object |

All four types are accepted by `compute.call()`, `compute.segment()`, `gpu.buffer()`, `gpu.dispatch()` buffer data, and `gpu.pipeline()` buffer data. This enables zero-copy data flow:

```lua
-- Disk → GPU (zero-copy via mmap)
local mapped = fs.mmap("embeddings.bin")
gpu.buffer("vectors", mapped)
mapped:close()

-- WASM → GPU (zero-copy via WasmBuffer)
local processed = compute.call("preprocess", raw_data, { buffer = true })
gpu.buffer("features", processed)  -- WasmBuffer accepted directly
```

C helper functions:
- **Lua:** `lua_get_buffer(L, idx, &view)`. Extracts `HlBufferView` from any buffer type at stack index
- **JS:** `js_get_buffer(ctx, val, &view, &str, &needs_free)`. Same for JS values

### Compute API Harmonization

WASM and GPU compute share symmetric naming where the concepts align:

| Concept | WASM (`compute.*`) | GPU (`gpu.*`) |
|---------|-------------------|---------------|
| Availability | `compute.available()` | `gpu.available()` |
| Load from file | `compute.load(name)` | `gpu.load(name)` |
| Execute | `compute.call(name, input)` | `gpu.dispatch(name, opts)` |
| Async execute | `compute.async.call(...)` | `gpu.async.dispatch(...)` |
| Persistent state | `compute.instance(name)` | `gpu.buffer(name, data)` |
| Shared data | `compute.segment(mod, seg, data)` | `gpu.buffer(name, data)` |
| Input types | string, WasmBuffer, MappedBuffer | string, WasmBuffer, MappedBuffer |
| Execution limits | Gas metering (per-instruction) | Timeout (5s wall clock) |

Intentionally different: `call` vs `dispatch` (function call vs hardware dispatch), `instance` vs `buffer` (retained linear memory vs GPU storage), `segment` vs `buffer` (read-only shared heap vs read/write GPU buffer).

## Terminal UI API surface

Design: [tui_mode.md](tui_mode.md). Contributor notes stay in `CLAUDE.md` ("Terminal UI module").


```lua
local tui = require("hull.tui")

-- Canonical entry point.
tui.run({
    draw     = function(t) ... end,        -- required, called every tick + on event
    on_event = function(ev) return nil end,-- nil = keep going; non-nil = exit token
    tick_ms  = -1,                         -- -1 = block until event; 100 = 10Hz
    mouse    = false,                      -- opt-in SGR mouse (CSI <btn>;<x>;<y> M)
    paste    = false,                      -- opt-in bracketed paste
    focus    = false,                      -- opt-in focus in/out events
    kitty_kbd= false,                      -- opt-in Kitty keyboard protocol
})

-- Helpers.
tui.list(items, opts?)        -- scrollable picker; returns picked index or nil
tui.confirm(msg)              -- y/N prompt; returns boolean
tui.input(prompt, opts?)      -- single-line editor with cursor + editing keys
tui.frame(opts, fn)           -- bordered area; opts.border ∈ single/double/round/ascii
tui.progress(pct, opts?)      -- "[████░░] 67%"
tui.spinner(state)            -- ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏. Returns frame + next state
tui.theme()                   -- "dark" | "light" | "unknown" (cached at acquire)
tui.caps()                    -- { truecolor, color256, color, mouse, focus, kitty_kbd, clipboard }
tui.clipboard_set(text)       -- OSC 52 write to system clipboard
tui.async(fn)                 -- spawn a detached coroutine on the event loop

-- Escape-hatch primitives (use tui.run instead).
tui.enter() / tui.leave()
tui.size() / tui.theme() / tui.caps()
tui.clear() / tui.invalidate() / tui.move(x, y) / tui.style(opts)
tui.print(x, y, s) / tui.write(s) / tui.flush()
tui.poll(timeout_ms)          -- yields to event loop; returns event or nil
```

JS API is the same shape (`import { tui } from "hull:tui"`) in camelCase: `tui.enableMouse`, `tui.clipboardSet`, `tui.poll` returns a `Promise<event|null>`.
