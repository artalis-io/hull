# HTMX + Pico app

Hypermedia-driven app scaffolded by `hull init --profile htmx`.

## First-run setup

```sh
make fetch-vendor   # downloads htmx + pico into static/vendor/ (SHA-pinned)
make dev            # starts hull dev server on :8080
```

`make fetch-vendor` is one-time: the assets get committed to your repo.
Re-run it (or `make verify-vendor`) when bumping HTMX or Pico versions
in the Makefile.

## What's wired in

- **HTMX** for partial page updates (full-page render on plain GET;
  fragment render on `HX-Request`).
- **Pico v2 classless** for default styling. Drop into `static/app.css`
  for custom rules.
- **Per-request CSP nonce** (`hull/web/middleware/csp@1` with the htmx
  profile: nonce-required for `<script>` and `<style>` blocks; inline
  `style="…"` attributes allowed for Pico's component styles).
- **Session-cookie storage** (basic, optional). Customize in `app.{lua,js}`.
- **CSRF protection** on unsafe methods (form posts + htmx requests).
  Change `secret = "CHANGE-ME-IN-PRODUCTION"` before deploying.

## Layout

```
app.{lua,js}             handlers + middleware
templates/
  base.html              <head>+<body> skeleton with nonce'd <script>/<link>
  pages/home.html        full-page render
  partials/
    todo_form.html       reusable form + CSRF token field
    todo_row.html        single-row fragment for htmx swaps
static/
  vendor/htmx.min.js          (fetched by make fetch-vendor)
  vendor/pico.classless.min.css   (fetched by make fetch-vendor)
  app.css                custom styles
migrations/
  001_init.sql           entries table
tests/
  test_app.{lua,js}      covers plain + htmx paths
```

## Pattern: when to return a fragment vs a full page

```
if htmx.is(req) then
    res:html(template.render("partials/foo.html", data))
else
    res:html(template.render("pages/foo.html", data))
end
```

See `docs/htmx.md` (in the Hull repo) for the full pattern guide.

## Next steps

- Replace `CHANGE-ME-IN-PRODUCTION` in `app.{lua,js}` with a real
  high-entropy secret loaded from env.
- Run `hull build` to produce a single binary with templates + static
  assets embedded.
