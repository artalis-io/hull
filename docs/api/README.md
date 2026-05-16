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

## Coverage

The **source files are the ground truth** — every public symbol in `include/hull/`,
`stdlib/lua/hull/`, and `stdlib/js/hull/` carries Doxygen / LDoc / JSDoc annotations.
The generated HTML (`make docs-api`) and the curated markdown files here are derived
views.

Source-level coverage (as of 2026-05-16):

- ✅ **C headers** (`include/hull/`) — `@file` block + per-function blocks for
  every public symbol:
  - Capability layer (`cap/*.h`): `db`, `fs`, `env`, `time`, `audit`, `body`,
    `crypto`, `http`, `smtp`, `image`, `ws`, `wasm`, `gpu`.
  - Runtime + core: `runtime.h`, `app_context.h`, `manifest.h`, `vfs.h`,
    `buffer.h`, `signature.h`, `release.h`.
  - Agent + commands: `agent_lib.h`, `agent_api.h`, `commands/dispatch.h`,
    every `commands/<verb>.h`.
- ✅ **Lua stdlib** (`stdlib/lua/hull/`) — `@module` + `@function`/`@tparam`/`@treturn`
  for every public function:
  - Top-level: `jwt`, `cookie`, `form`, `csv`, `validate`, `template`, `i18n`,
    `email`, `search`, `json`.
  - Middleware: `cors`, `auth`, `session`, `csrf`, `logger`, `ratelimit`,
    `transaction`, `idempotency`, `outbox`, `inbox`, `rbac`, `health`, `etag`.
- ✅ **JS stdlib** (`stdlib/js/hull/`) — same coverage with camelCase + JSDoc
  `@param`/`@returns`/`@example`.

Curated markdown (`c.md`, `lua.md`, `js.md`) is sampled, not exhaustive — for the
full surface use the generated HTML or read the annotated source directly.
