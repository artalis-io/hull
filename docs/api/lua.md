# Hull - Lua API Reference

Per-function reference for the Lua 5.4 stdlib (`stdlib/lua/hull/*.lua`).
Audience: app developers writing Lua applications on Hull.

For prose / patterns see [`../../CLAUDE.md`](../../CLAUDE.md) and
[`../agent_guide.md`](../agent_guide.md) § 8. For the JS counterpart
(same surface, camelCase) see [`js.md`](js.md).

## Table of contents

- [Routing & handlers](#routing--handlers) - `app.*`
  - [Request object (`req`)](#request-object-req)
  - [Response object (`res`)](#response-object-res)
- [Capability globals](#capability-globals)
  - [`db.*`](#dbtable-database)
  - [`http.*`](#httptable-outbound-http-client)
  - [`fs.*`](#fstable-filesystem)
  - [`crypto.*`](#cryptotable-crypto-primitives)
  - [`time.*`](#timetable-time)
  - [`env.*`](#envtable-env-vars-allowlist)
  - [`log.*`](#logtable-logging)
  - WebSocket, SSE, compute, gpu, image, smtp - _coming next_
- [Stdlib modules](#stdlib-modules)
  - [`hull.json`](#hulljson) · [`hull.web.cookie`](#hullcookie) · [`hull.jwt`](#hulljwt) · _and others coming_
- [Middleware modules](#middleware-modules)
  - [`hull.web.middleware.cors`](#hullmiddlewarecors) · _and others_

---

## Routing & handlers

### `app.get(pattern, handler)` / `post` / `put` / `delete` / `patch` / `head` / `options`

Register a route handler for the named HTTP method.

| Param     | Type                                 | Description |
|-----------|--------------------------------------|-------------|
| `pattern` | `string`                             | Path pattern. Supports exact paths (`/users`), parameters (`/users/:id`), and prefix wildcards (`/api/*`). |
| `handler` | `function(req, res)`                 | Handler called when a request matches. Free to be sync or use `hull.sleep`/`http.async.*` to yield. |

**Returns:** nothing.

**Throws:** if the same `(method, pattern)` is registered twice in the same load, the second call replaces the first (no error).

**Example:**

```lua
app.get("/users/:id", function(req, res)
    local user = db.query("SELECT * FROM users WHERE id = ?", { req.params.id })[1]
    if not user then res:status(404):json({ error = "not found" }); return end
    res:json(user)
end)
```

**See also:** [`app.use`](#appusemethod-pattern-mw), [`app.use_post`](#appuse_postmethod-pattern-mw).

---

### `app.use(method, pattern, mw)`

Register a **pre-body** middleware. Runs before the request body is read.
Suitable for auth, rate limiting, CORS, logging.

| Param     | Type                                 | Description |
|-----------|--------------------------------------|-------------|
| `method`  | `string`                             | HTTP method, or `"*"` to match any. |
| `pattern` | `string`                             | Path pattern. `"/*"` matches all paths. |
| `mw`      | `function(req, res) -> integer`      | Middleware function. Returns `0` to continue to the next middleware/handler, `1` to short-circuit (response already sent). |

**Returns:** nothing.

**Example:**

```lua
local cors = require("hull.web.middleware.cors").middleware({ origins = { "https://app.com" } })
app.use("*", "/api/*", cors)
```

---

### `app.use_post(method, pattern, mw)`

Register a **post-body** middleware. Runs after the request body has been
read (so it can inspect `req.body`). Suitable for CSRF, idempotency,
transaction wrapping.

Same signature as [`app.use`](#appusemethod-pattern-mw).

---

### `app.manifest(table)`

Declare the app's capability manifest. Must be called once, before any
route is registered.

| Param   | Type                | Description |
|---------|---------------------|-------------|
| `table` | `table`             | Manifest declaration. See below. |

**Manifest fields (all optional):**

| Field          | Type                                  | Description |
|----------------|---------------------------------------|-------------|
| `fs.read`      | `string[]`                            | Read grants: a directory, an exact file, or a single-`*` component pattern. See [Filesystem grants](#filesystem-grants). |
| `fs.write`     | `string[]`                            | Write grants (may create missing parents + the target). Same forms as `fs.read`. |
| `hosts`        | `string[]`                            | Outbound HTTP host allowlist for `http.*` / `ws.connect`. Supports `*.domain.com`. |
| `env`          | `string[]`                            | Env var names that `env.get` is permitted to read. Max 32 entries. |
| `gpu`          | `boolean` or `table{ devices = … }`   | Enable `gpu.*` global. `false` (default) hides it. |
| `cors`         | `table{ origins, methods, … }`        | Built-in CORS config (skips needing `hull.web.middleware.cors`). |
| `csp`          | `string`                              | Override the default Content-Security-Policy. |
| `wasm`         | `table{ heap, stack, gas, max_input, max_output }` | Per-app WASM limits (overrides CLI flags). |

**Returns:** nothing.

**Throws:** if called more than once or after any `app.get`/`use`/`ws`/`sse` call.

**Example:**

```lua
app.manifest({
    fs = { read = { "config.json", "data/*.csv" }, write = { "uploads/" } },
    hosts = { "api.example.com" },
    env = { "API_KEY", "STRIPE_SECRET" },
    gpu = true,
})
```

#### Filesystem grants

Each `fs.read` / `fs.write` entry is one of four forms, resolved relative to the
app root and confined to it (a symlink target that points outside the root is
re-rooted or clamped, never followed out):

| Grant | Example | Authorizes |
|-------|---------|------------|
| Base root | `.` or `./` | the **entire app directory and every descendant** (the broadest grant; use only when the app genuinely needs whole-dir access). Shadowed by any more specific grant. |
| Directory | `data/` or `data` | that directory and any descendant; in-root symlinks under it are followed, contained |
| Exact file | `config.json` | only that file; siblings are not reachable; a symlink at that name is refused |
| Write target (absent) | `out/result.bin` | creates the missing parent dirs and the file (`fs.write` only) |
| Pattern | `data/*.csv` | files whose name matches, **per component** |

All grant paths are **relative to the app root** and confined to it; an absolute
path (e.g. `/tmp/x` or `/etc/passwd`) is **rejected** at load - read/write an
external file by placing it under the app directory (or a bind mount) instead.

**Pattern rules (v1):** `*` matches zero or more bytes **within a single path
component** and never crosses `/`. Only `*` is supported; `?`, `[`, `]`, `{`, `}`,
`\`, and `**` are rejected with a manifest error. So `data/*.csv` allows
`data/a.csv` and `data/.csv`, but denies `data/a.txt`, `data/sub/a.csv` (the `*`
does not cross `/`), and `data/a.csv/x`. Multiple patterned components are allowed
(`logs/*/*.txt`). A pattern grant refuses symlinks in its matched portion (a
`data/link.csv` symlink is denied even if it matches), so a matching name cannot
alias a non-matching target.

**Security note:** the pattern is **enforced per file**. Before this, a
`data/*.csv` grant exposed the *entire* `data/` directory (only the kernel sandbox
enforced the grant, coarsely, at directory granularity). An app that was relying on
reading non-matching files under such a directory must now widen its manifest
(add the directory or the specific files); that access was unintended
over-authority, not a supported contract.

---

### Request object (`req`)

Passed to every handler / middleware as the first argument. Fields:

| Field        | Type      | Description |
|--------------|-----------|-------------|
| `req.method` | `string`  | HTTP method, uppercase (`"GET"`, `"POST"`, …). |
| `req.path`   | `string`  | Request path, without query string (`/users/42`). |
| `req.url`    | `string`  | Full URL including query (`/users/42?expand=1`). |
| `req.query`  | `table`   | Parsed query parameters. Keys are strings, values are strings (or arrays of strings for repeated keys). |
| `req.headers`| `table`   | Request headers. Keys are **lowercased**; values are strings. |
| `req.params` | `table`   | Captured route parameters from `:name` placeholders. |
| `req.body`   | `string`  | Raw body bytes. Reading it triggers body capture; available in `app.use_post` and handlers. Not available in `app.use` (pre-body). |
| `req.ctx`    | `table`   | Mutable per-request scratch space for middleware-to-handler data passing. Starts as `{}`. |

**Notes:** `req.body` for `application/x-www-form-urlencoded` requests is the raw URL-encoded string; use `require("hull.web.form").parse(req.body)` to decode. For JSON request bodies use `json.decode(req.body)`.

---

### Response object (`res`)

Passed as the second argument. Methods are chainable.

#### `res:status(code)`

Set the HTTP status code.

| Param  | Type      | Description |
|--------|-----------|-------------|
| `code` | `integer` | HTTP status (100–599). |

**Returns:** `res` (chainable).

---

#### `res:header(name, value)`

Set a response header.

| Param   | Type     | Description |
|---------|----------|-------------|
| `name`  | `string` | Header name. Case-preserved on the wire. |
| `value` | `string` | Header value. CRLF/NUL are rejected by Keel. |

**Returns:** `res` (chainable).

---

#### `res:json(value)`

Send a JSON response. Sets `Content-Type: application/json`.

| Param   | Type  | Description |
|---------|-------|-------------|
| `value` | `any` | Encoded via `hull.json`. Sorted keys for deterministic output. |

**Returns:** nothing (terminates the response - subsequent `res:*` calls on the same response are no-ops).

---

#### `res:text(string)` / `res:html(string)`

Send a plain-text / HTML response. Sets the appropriate Content-Type.

| Param | Type     | Description |
|-------|----------|-------------|
| `s`   | `string` | Body content. |

**Returns:** nothing.

---

#### `res:redirect(url, [code])`

Send a redirect response.

| Param  | Type      | Description |
|--------|-----------|-------------|
| `url`  | `string`  | Target URL. |
| `code` | `integer` | Status code (default `302`). |

**Returns:** nothing.

---

#### `res:cookie(name, value, opts)`

Set a cookie via `Set-Cookie`.

| Param   | Type     | Description |
|---------|----------|-------------|
| `name`  | `string` | Cookie name. |
| `value` | `string` | Cookie value. |
| `opts`  | `table`  | Options: `path`, `httponly`, `secure`, `samesite`, `max_age`, `domain`. Same set as `hull.web.cookie.serialize`. |

**Returns:** `res` (chainable).

---

#### `res:file(path)`

Send a file from disk via zero-copy `sendfile(2)`. Path is cap-validated
against the manifest's `fs.read`.

| Param  | Type     | Description |
|--------|----------|-------------|
| `path` | `string` | Relative path. |

**Returns:** nothing.

**Throws:** manifest-deny if `path` isn't allowed.

---

## Capability globals

### `db.*` table - Database

Requires `HL_ENABLE_DB=1` (default). In compute-only builds the global is
absent.

#### `db.query(sql, params?)`

Run a SELECT.

| Param    | Type     | Description |
|----------|----------|-------------|
| `sql`    | `string` | SQL with `?` placeholders. |
| `params` | `table?` | Array of parameter values. `string` / `number` / `boolean` / `nil` map to SQLite TEXT/INTEGER-or-FLOAT/(INTEGER 0|1)/NULL. |

**Returns:** `table[]` - array of row tables. Each row is a `{ column_name = value, ... }` table. Empty array if no rows.

**Errors:** raises a Lua error on prepare/bind/step failure. Use `pcall` to catch.

**Example:**

```lua
local users = db.query("SELECT id, name FROM users WHERE active = ?", { true })
for _, u in ipairs(users) do print(u.id, u.name) end
```

---

#### `db.exec(sql, params?)`

Run INSERT/UPDATE/DELETE/DDL.

| Param    | Type     | Description |
|----------|----------|-------------|
| `sql`    | `string` | SQL with `?` placeholders. |
| `params` | `table?` | Parameter values (same convention as `db.query`). |

**Returns:** `integer` - affected row count.

**Errors:** raises on failure.

---

#### `db.batch(fn)`

Run `fn` inside a `BEGIN IMMEDIATE ... COMMIT` transaction. Rollback on
error (the error is re-raised).

| Param | Type       | Description |
|-------|------------|-------------|
| `fn`  | `function` | Callback. May call `db.exec` / `db.query` freely. |

**Returns:** whatever `fn` returns.

**Errors:** if `fn` raises, the transaction is rolled back and the error is propagated to the caller. Nested `db.batch` is not supported (SQLite has no nested transactions); calling it inside another `db.batch` raises.

**Example:**

```lua
db.batch(function()
    db.exec("INSERT INTO orders (user_id, total) VALUES (?, ?)", { uid, total })
    db.exec("INSERT INTO audit (action) VALUES (?)", { "order_created" })
end)
```

---

#### `db.last_id()`

ROWID of the most-recently-inserted row on this connection.

**Returns:** `integer` - `last_insert_rowid()`, or `0` if no INSERT has occurred.

---

#### `db.async.query(sql, params?)`

Like `db.query` but yields to the event loop (dispatches to the worker
DB pool). Other requests are served while this one waits.

| Param    | Type     | Description |
|----------|----------|-------------|
| `sql`    | `string` | SQL with `?` placeholders. |
| `params` | `table?` | Parameters. |

**Returns:** same as `db.query` (rows array).

**Notes:** requires `--worker-db` configured. The worker pool uses the same SQLite file via a separate connection.

---

#### `db.async.exec(sql, params?)`

Async equivalent of `db.exec`.

**Returns:** affected row count.

---

#### `db.udf.register(name, fn, opts?)` / `db.udf.unregister(name)`

Register a user-defined SQL function backed by a Lua callback.

| Param  | Type                                | Description |
|--------|-------------------------------------|-------------|
| `name` | `string`                            | SQL function name. **Must start with `hull_`** (prevents shadowing SQLite built-ins). |
| `fn`   | `function` or `table{step, finalize}` | Scalar function or aggregate (step + finalize). |
| `opts` | `table?`                            | `{ deterministic = bool, args = int }`. `args = -1` means variadic. |

**Returns:** nothing.

**Throws:** if `name` doesn't start with `hull_`.

**Example:**

```lua
db.udf.register("hull_double", function(x) return x * 2 end,
                { deterministic = true, args = 1 })

local rows = db.query("SELECT id, hull_double(score) AS doubled FROM games")
```

---

(Continuing through `http`, `crypto`, `time`, `env`, `log`, then
stdlib modules and middleware. Representative slice complete for
review.)

---

### `fs.*` table - Filesystem

`require("hull.fs")`. Every path is relative to the app root, resolved through the
descriptor-relative virtual-root resolver + the compiled authorization policy: an
op selects from `fs.read` (read / stat / list / mmap) or `fs.write` (write), then
resolves the literal path under the selected grant's held anchor. Absolute paths
and `..` are rejected. See [Filesystem grants](#filesystem-grants).

#### `fs.read(path)` / `fs.write(path, bytes)` / `fs.mmap(path[, {offset, length}])`

`read` returns the whole file as a binary-safe string (or `nil, err`); `write`
creates missing parents + the file and returns `true` (or `nil, err`); `mmap`
returns a read-only `MappedBuffer` (optionally a page-aligned window).

#### `fs.stat(path)`

Return a metadata table, or `nil` when the path does not exist (so `fs.stat(p) ~=
nil` subsumes an `exists` check). On a policy / IO error returns `nil, err`.
**lstat semantics:** a terminal symlink is reported **as a link** (`type =
"symlink"`), never followed - a metadata op cannot alias a symlink target.

| Field   | Type      | Description |
|---------|-----------|-------------|
| `type`  | `string`  | `"file"`, `"dir"`, `"symlink"`, or `"other"` (FIFO / socket / device). |
| `size`  | `integer` | Size in bytes. |
| `mode`  | `integer` | Permission bits (`st_mode & 0o777`). |
| `mtime` | `integer` | Modification time, epoch seconds. Reproducible builds MUST NOT key on it. |

Requires `fs.read` authority over `path`. A directory is statable too, including
the app root itself - `fs.stat(".")` returns `{ type = "dir", ... }` under a
base-root (`.`) grant. A path that is authorized but does not exist (including a
grant whose parent directory is still absent) returns `nil`, not an error.

#### `fs.list(dir)`

Return a **deterministically ordered** array of `{ name, type, size }` (non-recursive;
`.` and `..` omitted), or `nil, err`. **Ordering** is unsigned-byte lexicographic,
shorter-prefix-first - identical on every platform, independent of locale and
`readdir` order. Each entry's `type` comes from an lstat, so a symlink child is
reported as `"symlink"`, never followed. An empty directory yields `{}`; a missing
directory yields `nil, "not_found"`.

Selection is gated by `fs.read`: a **directory** grant lists any descendant
directory; a single-terminal **pattern** grant (e.g. `data/*.csv`) lists its
directory exposing **only matching names**; an **exact-file** grant authorizes
that path but it is not a directory, so `list` of the exact file returns
`not_a_directory` while its **parent and siblings** (unauthorized) return
`permission`. When grants overlap, the **most specific** governs (a narrower grant
shadows a broader one, and a governing multi-component pattern such as
`logs/*/*.txt` denies `list("logs")` rather than falling through to a `logs/`
subtree grant).

**Error tokens** (Lua returns `nil, token`; JS throws with the token in the
message): `permission`, `invalid_path`, `not_found`, `not_a_directory`,
`symlink_denied`, `too_many_entries`, `listing_too_large`, `name_too_long`,
`size_unrepresentable`, `io_error`. A size beyond `2^53 - 1` errors
(`size_unrepresentable`) rather than returning a rounded number, so Lua and JS
agree exactly.

---

## Status

Initial slice complete for:
- `app.*` routing + `req` + `res`
- `db.*` (full surface)

The format is now concrete - please review before I batch-produce the
rest. Remaining work:

- Capability globals: `http`, `fs`, `crypto`, `time`, `env`, `log`,
  `ws`, `compute`, `gpu`, `image`, `smtp`, `hull` (the `hull.sleep` /
  `hull.gather` module)
- `app.*` continued: `app.ws`, `app.sse`, `app.every`, `app.daily`
- Stdlib modules: `json`, `cookie`, `jwt`, `template`, `validate`,
  `form`, `i18n`, `csv`, `search`, `email`, `image`
- Middleware modules: `cors`, `csrf`, `auth`, `session`, `ratelimit`,
  `logger`, `transaction`, `idempotency`, `outbox`, `inbox`, `rbac`,
  `health`, `etag`

Estimate: ~200 functions total. ~25 documented after this initial slice (12%).
