# Hull — JavaScript API Reference

Per-function reference for the JS stdlib (`stdlib/js/hull/*.js`).
Audience: app developers writing JavaScript applications on Hull.

For prose / patterns see [`../../CLAUDE.md`](../../CLAUDE.md) and
[`../agent_guide.md`](../agent_guide.md) § 8. For the Lua counterpart
(same surface, snake_case) see [`lua.md`](lua.md).

**Naming**: JS uses camelCase. Where an option exists in both Lua and JS
with a snake_case name in Lua, the JS side **accepts both** (`maxRows`
and `max_rows`, `includeHeaders` and `include_headers`).

## Table of contents

- [Routing & handlers](#routing--handlers) — `app.*`
  - [Request object (`req`)](#request-object-req)
  - [Response object (`res`)](#response-object-res)
- [Capability imports](#capability-imports) — `import { db, http, … } from "hull:…"`
  - [`db`](#db-database)
  - [`http`](#http-outbound-http-client)
  - WebSocket, SSE, compute, gpu, image, smtp, crypto, time, env — _coming next_
- [Stdlib modules](#stdlib-modules)
- [Middleware modules](#middleware-modules)

---

## Routing & handlers

### `app.get(pattern, handler)` / `post` / `put` / `delete` / `patch` / `head` / `options`

Register a route handler.

| Param     | Type                                  | Description |
|-----------|---------------------------------------|-------------|
| `pattern` | `string`                              | Path pattern. Supports exact paths (`"/users"`), parameters (`"/users/:id"`), and prefix wildcards (`"/api/*"`). |
| `handler` | `(req, res) => void` or `async (...)` | Handler. May be sync or async (use `await http.async.*` / `await hull.sleep`). |

**Returns:** `undefined`.

**Example:**

```javascript
app.get("/users/:id", async (req, res) => {
    const rows = db.query("SELECT * FROM users WHERE id = ?", [req.params.id]);
    if (rows.length === 0) { res.status(404).json({ error: "not found" }); return; }
    res.json(rows[0]);
});
```

---

### `app.use(method, pattern, mw)`

Register pre-body middleware. Same semantics as Lua.

| Param     | Type                                | Description |
|-----------|-------------------------------------|-------------|
| `method`  | `string`                            | HTTP method or `"*"`. |
| `pattern` | `string`                            | Path pattern. |
| `mw`      | `(req, res) => number`              | Returns `0` to continue, `1` to short-circuit. |

---

### `app.usePost(method, pattern, mw)`

Register post-body middleware. Same shape as `app.use`. **Note the
camelCase** — `app.usePost`, not `app.use_post`.

---

### `app.manifest(config)`

Declare the app's capability manifest. Must be called once before any
route is registered.

| Param    | Type     | Description |
|----------|----------|-------------|
| `config` | `object` | Manifest fields. See below. |

**Manifest fields:**

| Field         | Type                                       | Description |
|---------------|--------------------------------------------|-------------|
| `fs.read`     | `string[]`                                 | Allowed read paths. |
| `fs.write`    | `string[]`                                 | Allowed write paths. |
| `hosts`       | `string[]`                                 | Outbound HTTP / WS host allowlist. |
| `env`         | `string[]`                                 | Env var allowlist. |
| `gpu`         | `boolean` or `{ devices: number[] }`       | Enable `gpu` import. Defaults to `false`. |
| `cors`        | `{ origins, methods, headers, credentials, maxAge }` | Built-in CORS. |
| `csp`         | `string`                                   | Override default CSP. |
| `wasm`        | `{ heap, stack, gas, maxInput, maxOutput }` | Per-app WASM limits. |

**Returns:** `undefined`.

**Example:**

```javascript
app.manifest({
    fs: { read: ["config.json"], write: ["uploads/"] },
    hosts: ["api.example.com"],
    env: ["API_KEY"],
    gpu: true,
});
```

---

### Request object (`req`)

| Field         | Type     | Description |
|---------------|----------|-------------|
| `req.method`  | `string` | HTTP method, uppercase. |
| `req.path`    | `string` | Path without query string. |
| `req.url`     | `string` | Full URL including query. |
| `req.query`   | `object` | Parsed query params. Values are strings or string arrays. |
| `req.headers` | `object` | Request headers. **Keys are lowercased**. |
| `req.params`  | `object` | Captured route parameters. |
| `req.body`    | `string` | Raw body. Available in `usePost` middleware and handlers, not in `use`. |
| `req.ctx`     | `object` | Per-request scratch space (`{}` initially). |

#### `req.header(name)`

Helper to read a header case-insensitively.

| Param  | Type     | Description |
|--------|----------|-------------|
| `name` | `string` | Header name; case-insensitive. |

**Returns:** `string \| undefined`.

---

### Response object (`res`)

#### `res.status(code)`

Set status code. Chainable (returns `res`).

#### `res.header(name, value)`

Set response header. Chainable.

#### `res.json(value)`

Send JSON response. `Content-Type: application/json`. Terminates the response.

#### `res.text(string)` / `res.html(string)`

Send plain-text / HTML response. Terminates.

#### `res.redirect(url, code?)`

Send redirect. Default `code` = `302`.

#### `res.cookie(name, value, opts?)`

Set cookie. Options: `path`, `httpOnly`, `secure`, `sameSite`, `maxAge`, `domain`. Chainable.

#### `res.sendFile(path)`

Zero-copy `sendfile` from disk. Path validated against manifest's `fs.read`.

---

## Capability imports

JS uses ES modules. All Hull capabilities are imported by name:

```javascript
import { db } from "hull:db";
import { httpClient } from "hull:http-client";
import { fs } from "hull:fs";
import { crypto } from "hull:crypto";
import { time } from "hull:time";
import { env } from "hull:env";
import { log } from "hull:log";
```

Modules are loaded on first import; subsequent imports return the same
binding.

### `db` — Database

Requires `HL_ENABLE_DB=1`. The import fails to resolve in compute-only
builds.

#### `db.query(sql, params?)`

Run a SELECT.

| Param    | Type            | Description |
|----------|-----------------|-------------|
| `sql`    | `string`        | SQL with `?` placeholders. |
| `params` | `any[]?`        | Array of parameter values. `string` / `number` / `boolean` / `null` / `ArrayBuffer` map to SQLite TEXT/INTEGER-or-FLOAT/(0|1 integer)/NULL/BLOB. |

**Returns:** `object[]` — array of row objects. Each row is `{ column_name: value, ... }`. Empty array if no rows.

**Throws:** on prepare/bind/step failure.

**Example:**

```javascript
const users = db.query("SELECT id, name FROM users WHERE active = ?", [true]);
for (const u of users) console.log(u.id, u.name);
```

---

#### `db.exec(sql, params?)`

Run INSERT/UPDATE/DELETE/DDL.

| Param    | Type     | Description |
|----------|----------|-------------|
| `sql`    | `string` | SQL with `?` placeholders. |
| `params` | `any[]?` | Parameter values. |

**Returns:** `number` — affected row count.

**Throws:** on failure.

---

#### `db.batch(fn)`

Run `fn` inside a `BEGIN IMMEDIATE ... COMMIT` transaction.

| Param | Type                              | Description |
|-------|-----------------------------------|-------------|
| `fn`  | `() => any` or `async () => any`  | Callback. |

**Returns:** whatever `fn` returns.

**Throws:** on `fn` failure (transaction rolled back, error re-thrown). Nested `batch` is not supported.

---

#### `db.lastId()`

| Returns | `number` — last `ROWID` from an INSERT on this connection, or `0`. |

---

#### `db.async.query(sql, params?)` / `db.async.exec(sql, params?)`

Async variants — return `Promise<object[]>` / `Promise<number>` and yield
to the event loop. Other requests are served while these wait.

**Example:**

```javascript
const rows = await db.async.query("SELECT * FROM large_table");
```

---

#### `db.udf.register(name, fn, opts?)` / `db.udf.unregister(name)`

Register a JS-backed UDF.

| Param  | Type                                  | Description |
|--------|---------------------------------------|-------------|
| `name` | `string`                              | Must start with `hull_`. |
| `fn`   | `function` or `{step, finalize}`      | Scalar or aggregate. |
| `opts` | `{ deterministic, args }`             | `args = -1` means variadic. |

**Throws:** if `name` doesn't start with `hull_`.

---

(Continuing through `http`, `fs`, `crypto`, etc. Representative slice
complete.)

---

## Status

Initial slice covers `app.*` routing, `req`/`res`, `db` (full surface).
Remaining work parallels the Lua doc. Estimate: ~200 functions total.
~25 documented after this slice.

The format is now concrete — please review before I batch-produce the rest.
