# htmx_widgets_register

Tiny asset-register CRUD app that exercises **every widget** in the
§1.5.g htmx tier in one screen. Doubles as:

1. **Adoption reference** — read `app.lua` (≈210 lines) to learn the
   pattern for composing search + sort + pagination + inline-edit +
   form + confirm + toast into a single page.
2. **UX-test surface** — spin it up and click through the checklist
   below to verify every widget behaves correctly. Faster than
   running a headless-browser test suite.

## Run it

From the repo root, after `make`:

```sh
cd examples/htmx_widgets_register
make dev
# open http://127.0.0.1:8080/
```

The Makefile uses a temp DB at `$TMPDIR/hull-htmx-widgets.db` so
successive runs start with the same seeded 20 rows. To wipe + start
clean: `make clean && make dev`.

## What gets exercised

| Widget | Where in the UI | What to verify |
|---|---|---|
| `search` | Top input | Type → debounced fetch → grid swaps. Clear button (X) also clears + refreshes. URL pushes `?q=...` |
| `sort` | Click any column header | Arrow appears (▲ asc, ▼ desc, ↕ none). Click again → flips direction. Click another column → resets to asc. `?sort=col:dir` in URL |
| `pagination` | Bottom of grid | Click page numbers / `←` / `→` — grid swaps. Active page has no link. URL pushes `?page=N` |
| `inline-edit` | Click the **Name** or **Category** cell | Cell becomes an editable input + Save/Cancel. Enter or Save submits PATCH. Esc or Cancel reverts. Editor input is focused + text-selected on swap |
| `table` | The grid itself | All the above wired together. Custom render for the **Status** column shows colored badges |
| `form` | "Add asset" section at bottom | Submit empty → server validates → `aria-invalid="true"` on the failing input + inline error span. Submit button shows "Adding…" + `aria-busy` during submission |
| `confirm` | Click any `× <id>` button under the table | Styled `<dialog>` opens (not the browser popup). Cancel button focused; Esc or backdrop click cancels. Delete button is danger-styled |
| `toast` | After Add / Edit / Delete | Top-right corner gets a toast: success-green for create + delete, info-blue for inline-edit save. Click `×` or wait 4s to auto-dismiss |

## UX-test checklist

Click through these in order. Each step exercises one widget end-to-end.

- [ ] **search**: Type `pump` in the search box → grid filters to one row in ~250ms. Clear → all rows return. URL shows `?q=pump`.
- [ ] **sort**: Click the `Name` header → rows sort A-Z, ▲ arrow appears. Click again → Z-A, ▼ arrow. Click `Category` → resets to ▲ on Category. URL shows `?sort=name:asc` etc.
- [ ] **pagination**: With 20 seeded rows + per-page=8, you should see pages 1 2 3 with ellipsis. Click page 2 → grid swaps to rows 9-16. URL shows `?page=2`.
- [ ] **search + sort + pagination compose**: Type `electric` → 6 rows match (single page, no nav). Clear → 20 rows + pages return. Now sort by Name desc → click page 2 → URL has both `?sort=name:desc&page=2`.
- [ ] **inline-edit**: Click the `Pump #7` text. It becomes an editable input. Type `Pump #99` → press Enter. Cell saves + flips back to display mode with the new value. Top-right shows "name updated" toast.
- [ ] **inline-edit cancel**: Click any name → press Esc. Reverts to original value, no toast.
- [ ] **inline-edit empty**: Click any name → clear the input → press Enter. Editor re-renders with "(cannot be empty)" hint.
- [ ] **form (success)**: Fill `Name` + `Category` in the "Add asset" form → click Add. Button briefly shows "Adding…" + disabled. Grid refreshes with the new row + green toast "Added X".
- [ ] **form (validation)**: Clear `Name` → click Add. Form re-renders with the error span under Name input. `aria-invalid="true"` set; submit button is back to normal (no leftover busy state).
- [ ] **confirm**: Click `× 1` under the table. Styled `<dialog>` appears: title-less, message "Delete this asset?…", Cancel + Delete (red) buttons. Esc dismisses (no delete). Click `× 1` again → click backdrop → dismisses. Click `× 1` → click Delete → row gone + green toast "Deleted Pump #99".
- [ ] **toast a11y**: Trigger a successful save with `prefers-reduced-motion` enabled (system setting). Toast appears without animation but still announces via `aria-live="polite"`. Error-level toasts use `role="alert"` for assertive announcement.

## Architecture notes (for the code reading)

- **Two SQL columns are editable** (`name`, `category`). The single `/assets/:id/:col/edit` + `PATCH /assets/:id/:col` route pair handles both via the `:col` param, validated against an allowlist (`valid_col`).
- **Status column** uses the `schema.render` callback to emit `<span class="badge badge-active">active</span>` — shows how to wrap custom HTML around values while keeping the rest of the table-render machinery in play.
- **Pre-rendered widget strings.** Hull's template engine doesn't support function calls in `{{ }}`; widget helpers run server-side and the strings flow into the template data table. The two static ones (`SEARCH_INPUT_ATTRS`, `DELETE_CONFIRM_ATTRS`) are pre-rendered at module load.
- **Search preserves through sort/pagination.** When `?q=foo` is active, the `base_url` for sort headers and pagination links is `/search?q=foo`, so toggles stay filtered.
- **Delete buttons live in an `<div class="action-bar">` below the table**, not in a "Actions" column. This keeps `table.render` focused on the data grid; per-row actions are handled at the page level. (A future "actions column" pattern is possible via `schema.render` returning a button.)

## Module declarations

The app's manifest declares 17 modules — every widget in the tier
plus its transitive dependencies (Hull's resolver pulls in transitive
deps automatically when a wider module like `hull/web/htmx/table@1`
declares them, but the example lists them explicitly for clarity).

## What this example is NOT

- **Not a JS sibling.** A Lua-only example. The widget tier has JS
  sibling modules (same surface, camelCase names); a parallel
  `app.js` would be a straight transliteration. Add it when there's
  demand.
- **Not authenticated.** No session/cookie/CSRF — the example focuses
  on the widget UX. For an auth-wired hypermedia app see
  `examples/hypermedia_photos/`.
- **Not internationalized.** Strings are English-only. The widgets
  themselves are i18n-friendly via `opts.label` / `opts.yes` / etc.
