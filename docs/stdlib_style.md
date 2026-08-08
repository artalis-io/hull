# Hull stdlib style guide

The Hull stdlib is dual-runtime: every user-facing module is written once in
Lua (`stdlib/lua/hull/`) and once in JS (`stdlib/js/hull/`), kept at **semantic**
parity. This guide is the house style those modules follow. It exists because a
2026-08 review found the stdlib had grown four different error conventions, the
same concept named eight ways, and a handful of avoidable footguns. New and
changed stdlib code MUST follow the rules here; existing code is reconciled to
them incrementally.

The audience is contributors to the stdlib, not app authors. (App-facing docs
live per-module and in `CLAUDE.md`.)

---

## 1. Error handling

**The rule.** *Exceptions represent failure to perform the requested operation.
Values represent successful outcomes, including expected negative outcomes such
as absence.*

- **Failure → throw.** `error(msg)` in Lua, `throw new Error(msg)` in JS. A
  failure is: invalid input, a violated precondition (missing required option,
  wrong type), an exceeded cap, or a backend/transport error.
- **Absence is a value, not an error.** A lookup / load / cache read that finds
  nothing returns `nil` / `null`. "Not found" is a normal outcome the caller
  routinely branches on; it is not a failure to perform the operation.
- **Predicates return their value.** A boolean check returns a boolean; a
  counter returns a number. No wrapping.
- **Structured domain results are allowed** *only* when the structure is
  semantically part of a **successful** operation (e.g. a validator's findings).
  They are never a general-purpose error transport. Do not return
  `(value, err)` / `{ ok, error }` as the standard way to signal failure.

**Throwing is not HTTP.** A thrown error propagates to the enclosing execution
boundary. An HTTP handler *may* map an unhandled failure to a 500, but the
stdlib itself is transport-agnostic — a CLI or a job runner is an equally valid
boundary. Never phrase a stdlib contract in terms of HTTP status.

### Outcome categories

| Operation | Outcome | Result |
|---|---|---|
| `session.load(id)` | found / not-found / DB down | object / `nil` / **throw** |
| a cache `get(k)` | hit / miss / backend error | value / `nil` / **throw** |
| `csv.parse(input)` | valid / malformed | parsed value / **throw** |
| `validate.check(data, schema)` | valid / invalid / validator broken | `(true, {})` / `(false, findings)` / **throw** |
| `email.send(msg)` | sent / missing arg / transport fail | `true` / **throw** / **throw** |

**Validation failure ≠ validator failure.** An *invalid input* is a normal,
successful outcome of running the validator: it returns `(false, findings)`. The
*validator itself* failing (a bad schema, an exception in a custom rule) throws.

### Structured error identity

Callers must not have to match on message strings (`err:find("constraint")`).
Errors that a caller might reasonably branch on carry a **stable `.code`**:

```lua
local ok, err = pcall(function() db.exec(...) end)
if not ok and type(err) == "table" and err.code == "constraint_violation" then ...
```
```js
try { db.exec(...); } catch (e) { if (e.code === "constraint_violation") { ... } }
```

Codes are lower-snake, stable across releases, and documented on the throwing
function. A bare `error("message")` is fine where no caller branches on it;
promote to a coded error the moment one does. (Retrofitting existing throws with
codes is incremental — add a code when a branch needs it.)

### Lua/JS: semantic, not syntactic, equivalence

Keep the *meaning* identical across runtimes; do not force identical surface
syntax. Multiple return values are idiomatic in Lua and clumsy in JS, so a
validator is `(valid, errors)` in Lua and *may* be `{ valid, errors }` in JS.
What must match is the semantics: same categories, same codes, same "throws vs
returns nil vs returns data" decision.

---

## 2. Naming

- **snake_case in Lua, camelCase in JS**, and the mapping must be faithful and
  total — every option/field/function present in one runtime is present in the
  other under the case-translated name. A name in one runtime and not the other
  (or spelled differently beyond case) is a parity bug.
- **Time is seconds, and lifetimes are `*_ttl`.** Prefer `ttl` / `<thing>_ttl`
  (`verify_ttl`, `state_ttl`) over `max_age` / `expires` / `lifetime` /
  `retention` for the same concept. Where a value is genuinely a wall-clock
  instant (a cookie `Expires`), name it `expires` and document the unit; where
  it is a duration, it is seconds unless the name says otherwise (`retain_days`).
- **One name per concept across modules that wire together.** The HMAC key an
  app passes is `secret` everywhere (not `state_secret` in one middleware and
  `secret` in its sibling). The session cookie is `name` (with a documented
  default), not `cookie_name` here and `name` there. When two modules are meant
  to be configured side by side, their shared options share a spelling.
- Introducing an alias for back-compat is acceptable (accept the old name, treat
  the canonical as primary in docs); introducing a *new* clash is not.

---

## 3. Footguns to avoid

- **No require-time capability acquisition.** Do not call
  `require("hull.db").default()` (or any capability that resolves post-startup)
  at module top level. `db.default()` only resolves after the manifest is
  applied, so a top-level bind fails or captures the wrong connection depending
  on require order. Acquire lazily inside `init()` / the middleware factory /
  the handler.
- **`or` defaulting differs between the runtimes — mind which value it drops.**
  In Lua only `nil` and `false` are falsy, so `opts.x or default` keeps `0` and
  `""` (unlike JS `||`, which drops them) but silently drops a legitimate
  `false` — a real bug for a boolean option (`opts.secure or true` can never be
  `false`). In JS `opts.x || default` drops `0`, `""`, AND `false`. For any
  option whose valid values include `0`/`""`/`false`, use an explicit nil check
  — Lua `opts.x ~= nil and opts.x or default` / `if opts.x == nil then`, JS
  `opts.x !== undefined ? opts.x : default`. The same option must default
  identically in a module's `init` and its `middleware`, and across runtimes.
- **No process-global mutable state for request-scoped concerns.** A server
  handles many requests on one runtime; a module-level `active_locale` set by
  one request leaks into the next. Thread request-scoped state through `req.ctx`,
  not a file-scoped variable.
- **No in-place mutation of caller-owned tables.** A function named `check` /
  `parse` / `render` must not write back into the table it was handed
  (`validate.check` trimming into `data[field]` is surprising). Return a new
  value.
- **Docs match code.** A default documented as `false` must not be `true` in the
  code (`cookie.secure`). A "silently dropped" in a docstring must not be an
  `error()` in the body (`csv.parse`).

---

## 4. DRY: shared internal helpers

The stdlib is dual-runtime, so load-bearing logic inherently exists twice. Do
not *also* duplicate it within a runtime. When the same non-trivial helper
appears in two modules, it moves to a shared internal module:

- Internal (contributor-only, not app-declarable) modules are `_`-prefixed:
  `hull.web._request`, `hull.web._html`, etc. They are required by other stdlib
  modules, never declared by apps.
- Security-relevant helpers (HTML escaping, HMAC/hex, client-IP extraction)
  especially must live in exactly one place per runtime — a forked copy that
  drifts is a latent vulnerability (an escape helper that lost its nil-guard, a
  hex helper that UTF-8-inflates high bytes).
- A shared helper still exists twice (Lua + JS). Guard the pair with a
  **cross-runtime parity test** (see `tests/e2e_template_parity.sh`) so the two
  copies cannot silently diverge.

Current shared helpers: `hull.web.htmx.escape` (HTML escaping for the widgets),
`hull.crypto.envelope` (signed token framing), `hull.web.cookie` (parse/
serialize), `hull.crypto._hex` (raw-byte hex - deliberately NOT
`crypto.hex_encode`/`hexEncode`, which UTF-8-inflates high bytes on the JS side;
see that module), and `hull.web._request` (`client_ip(req, trust_proxy)`:
XFF-first when trusted, `remote_addr` fallback, 64-char cap - the canonical
source for the client IP that `session` / `audit-log` / `totp` / `auth-flows`
each used to hand-roll subtly differently).

---

## 5. Testing parity

Any behavior that is (a) hand-ported across both runtimes and (b) security- or
correctness-sensitive gets a **golden cross-runtime parity test**: render/compute
the same inputs through Lua and JS and assert byte-identical output. This is the
mechanism that keeps two hand-maintained copies honest;
`tests/e2e_template_parity.sh` is the template for it.
