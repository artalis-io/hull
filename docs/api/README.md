# Hull — API Reference

Per-function reference for Hull's public API surfaces. Organised by language:

- [`c.md`](c.md) — C public headers (`include/hull/*.h`). For embedders, contributors, and runtime authors. ~250 functions across 29 headers.
- [`lua.md`](lua.md) — Lua 5.4 stdlib (`stdlib/lua/hull/*.lua`). For app developers writing Lua apps. ~200 functions across 28 modules (including middleware).
- [`js.md`](js.md) — JavaScript stdlib (`stdlib/js/hull/*.js`). For app developers writing JS apps. ~200 functions, JS↔Lua parity.

## Format

Every documented function follows the same template:

````
### `function_name(...)`

One-line summary. (What it does.)

| Param   | Type         | Description                              |
|---------|--------------|------------------------------------------|
| `name`  | `string`     | What this argument means                 |

**Returns:** `type` — what the return value contains, when it's nil/null, error conditions.

**Throws / errors:** when this can fail and how.

**Since:** `v0.X` (if relevant).

**Example:**

```lua
local r = db.query("SELECT id FROM users WHERE active = ?", { true })
```

**See also:** [`related_fn`](#related_fn).
````

For C functions the table uses C type names; "Returns" uses the same; "Throws/errors" maps to the function's error-code convention (typically `0` ok, `-1` error, or a domain-specific `HL_*_ERR_*` enum).

For Lua, methods on userdata use the `obj:method()` form; module functions use `module.fn()`. The signature uses the runtime's natural calling convention.

For JS, ESM imports are shown for stdlib modules (`import { foo } from "hull:foo"`); the signature uses camelCase.

## Naming conventions

| Language | Casing | Module form |
|---|---|---|
| C | `hl_<module>_<verb>` | header `<module>.h`, file `cap/<module>.c` etc. |
| Lua | `snake_case` | `require("hull.module")` returns a table; methods on userdata use `:` |
| JS | `camelCase` | `import { module } from "hull:module"` |

Where the same option exists in both Lua and JS, both casings are accepted (e.g. `maxRows` / `max_rows` in `csv.parse`).

## How this differs from the prose docs

- [`CLAUDE.md`](../../CLAUDE.md) and [`agent_guide.md`](../agent_guide.md) describe **behaviour, patterns, and intent** — the "why" and "how".
- This API reference is the **per-function lookup** — the "what does this exact thing take and return".
- Both reference each other; this one is grep-friendly for "find me the signature of X".

## Coverage status

Initial pass (2026-05-16): the docs cover the core surfaces. As of this writing:

- ✅ C: capability layer (`hl_cap_*`) — DB, FS, crypto, HTTP, env, time, audit
- ✅ Lua: globals (`app`, `db`, `http`, `fs`, `crypto`, `time`, `env`, `log`), top middleware
- ✅ JS: same surfaces, camelCase
- 🟡 Remaining: runtime internals (`HlRuntime`, `HlAppContext`), agent library API, less-common stdlib modules

See the per-file TOCs for what's complete.
