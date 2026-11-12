# Hull TypeScript declarations

Declarations for Hull's JavaScript surface: the `hull:*` modules, the `app` intrinsic,
the globals, and the shared shapes they pass around. Editors and `tsc --noEmit` use them
to type-check a Hull app written in JavaScript (`checkJs`) or TypeScript. Hull itself never
reads them. `hull types` installs them in an app; they are versioned with the hull binary
that embeds them. Design: [docs/typescript_design.md](../../docs/typescript_design.md) §2.

## Layout

| File | Declares |
|---|---|
| `globals.d.ts` | the globals app code sees: `hull`, `console` |
| `core.d.ts` | the shared `Hull` namespace: request / response, handlers, middleware, the manifest, the `app.main` context, database connections, buffers, refusal errors |
| `app.d.ts` | `declare module "hull:app"` |
| `modules/<a>.<b>.d.ts` | one `declare module "hull:a:b"` per registry module (`hull/web/middleware/cors` -> `modules/web.middleware.cors.d.ts`) |
| `../hull-test/test.d.ts` | the `test` global of `hull test` files, kept apart so app code does not see it (the `tests/tsconfig.json` `hull types` writes includes it) |

## Conventions

- Every app-importable registry module has exactly one `declare module "hull:<name>"`;
  internal modules (`hull:_template`, `hull:db:_internal`, `hull:ssh:wire`, ...) are never
  declared. A module that is not a JavaScript import (`hull:timers`, `hull:web:sse`,
  `hull:inspect`) is not declared.
- Declare what the runtime exports, exactly: the export names and every member the module
  objects carry. `make check-types` compares them with the running hull (drift check).
- Names follow the JS API: camelCase.
- Shared shapes live in `declare namespace Hull { ... }` (global, merged across files);
  a shape only one module uses is an exported `interface` / `type` inside its
  `declare module` block.
- No `any`, with one exception: `Hull.Data` (`= any`), for a value whose shape the APP
  defines - a database row field (`Hull.DataRow`), a decoded JSON body or payload, a job
  input / result, a signal payload, a `req.ctx` slot, `WsConnection.data`, session data.
  It is typed `any` as `JSON.parse`'s result is; where the API can take a type parameter,
  `Hull.Data` is its default (`conn.query<R>`, `json.decode<T>`, `test.get<T>`,
  `jobs.handler<D>`), so a caller who wants checking passes one. Everything the API itself
  defines stays precise: `Hull.Json` for JSON the API produces, `DbParam` for bound
  parameters, option bags, results with fixed shapes. Elsewhere use `unknown` for a value
  the caller must narrow, and a precise union where the runtime accepts a fixed set. Option bags are interfaces
  with optional members; required options are required.
- A function that parks (returns a Promise) is declared returning `Promise<T>`. A function
  that throws on failure is declared returning its success type; a function that returns
  an error value (`[value, err]`, `{ ok, error }`) declares that shape.
- Module-conditional members (`app.every` / `app.daily` need `hull/timers`; `app.get` and
  the other routes need `hull/http-server`) are declared, with the requirement in their
  doc comment.
- Each declaration carries a one-line `/** ... */` doc comment; longer explanations belong
  in docs/app_api_reference.md, which the comment may point to.
- `lib` is ES2023 with no DOM: do not use DOM types (`Request`, `Response`, `Blob`,
  `URL`, `TextEncoder`, `fetch`, ...); QuickJS does not have them.

## Verifying

`make check-types` (needs node; installs the pinned TypeScript from
`tests/types/package-lock.json`):

1. the declarations compile under `strict`;
2. the drift check: `tests/types/inventory.sh` loads every registry module in the built hull
   and records its live exports; `tests/types/drift.mjs` turns that into type-level
   assertions tsc checks against these files (missing, extra, or wrong-kind members fail);
3. `tests/types/fixtures/*.ts` use the API as an app would, with `// @ts-expect-error` on
   calls that must be rejected;
4. every example app (`examples/*`) is type-checked as JavaScript (`checkJs`).
