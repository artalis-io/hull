<!-- minimal -->
## Templates

HTML template engine with compile-once caching. Templates live in `app_dir/templates/`.

```lua
-- Lua
local template = require("hull.template")
local html = template.render("pages/home.html", { title = "Hello", user = { name = "Alice" } })
```

```javascript
// JS
import { template } from "hull:template";
const html = template.render("pages/home.html", { title: "Hello", user: { name: "Alice" } });
```

**Syntax:**
- `{{ var }}` - HTML-escaped output
- `{{ var.path }}` - dot path lookup (nil-safe)
- `{{{ var }}}` - raw/unescaped output
- `{{ var | filter }}` - apply filter
- `{% if cond %}` ... `{% elif %}` ... `{% else %}` ... `{% end %}`
- `{% for item in list %}` ... `{% end %}`
- `{% extends "base.html" %}` / `{% block name %}` ... `{% end %}`
- `{% include "partial.html" %}`
- `{# comment #}`

<!-- compact -->
## API

- **`template.render(name, data)`** - load, compile, render (cached after first call)
- **`template.render_string(source, data)`** / `template.renderString(...)` - compile from string
- **`template.compile(name)`** - returns compiled function for repeated rendering
- **`template.clear_cache()`** / `template.clearCache()` - clear compiled cache

## Filters

Chain with `|`: `{{ name | trim | upper }}`

| Filter | Effect |
|--------|--------|
| `upper` | Uppercase |
| `lower` | Lowercase |
| `trim` | Strip whitespace |
| `length` | String/array length |
| `default: "val"` | Fallback if nil |
| `json` | JSON-encode |
| `raw` | Skip HTML escaping |

## Template Inheritance

```html
<!-- templates/base.html -->
<html>
<head><title>{% block title %}Default{% end %}</title></head>
<body>{% block content %}{% end %}</body>
</html>

<!-- templates/pages/home.html -->
{% extends "base.html" %}
{% block title %}Home{% end %}
{% block content %}<h1>Welcome</h1>{% end %}
```

Multi-level inheritance is supported. Circular extends are detected and rejected.

## Includes

```html
{% include "partials/nav.html" %}
```

Included templates share the same data context as the parent.

## Gotchas

- **XSS safe by default:** `{{ }}` escapes `& < > " '`. Use `{{{ }}}` or `| raw` only for trusted HTML.
- **Dot paths are nil-safe:** `{{ user.address.city }}` returns empty string if any part is nil.
- **Lua truthiness:** `{}` and `0` are truthy in Lua. Use `has_items = #items > 0` for emptiness checks.
- **For-loop scoping:** Inside `{% for item in items %}`, `item` shadows any `data.item`.

<!-- full -->
## Full Page Example

```html
<!-- templates/base.html -->
<!DOCTYPE html>
<html>
<head>
    <title>{% block title %}My App{% end %}</title>
    <link rel="stylesheet" href="/static/style.css">
</head>
<body>
    {% include "partials/nav.html" %}
    <main>{% block content %}{% end %}</main>
</body>
</html>

<!-- templates/pages/users.html -->
{% extends "base.html" %}
{% block title %}Users{% end %}
{% block content %}
    <h1>Users ({{ users | length }})</h1>
    {% if users %}
        <ul>
        {% for user in users %}
            <li>{{ user.name }} &mdash; {{ user.email }}</li>
        {% end %}
        </ul>
    {% else %}
        <p>No users found.</p>
    {% end %}
{% end %}

<!-- templates/partials/nav.html -->
<nav>
    <a href="/">Home</a>
    {% if current_user %}
        <span>{{ current_user.name }}</span>
    {% else %}
        <a href="/login">Login</a>
    {% end %}
</nav>
```

## Rendering from Handler

```lua
app.get("/users", function(req, res)
    local users = db.query("SELECT name, email FROM users ORDER BY name")
    local html = template.render("pages/users.html", {
        users = users,
        current_user = req.ctx.session
    })
    res.html(html)
end)
```

```javascript
app.get("/users", (req, res) => {
    const users = db.query("SELECT name, email FROM users ORDER BY name");
    const html = template.render("pages/users.html", {
        users,
        currentUser: req.ctx.session
    });
    res.html(html);
});
```

## For-Loop with Key/Value

```html
{% for key, val in config %}
    <dt>{{ key }}</dt><dd>{{ val }}</dd>
{% end %}
```

## CSP Nonce

Pass nonce as data -- no special engine support needed:

```lua
local html = template.render("page.html", { csp_nonce = nonce })
```

```html
<script nonce="{{ csp_nonce }}">...</script>
```

## Render from String

```lua
local html = template.render_string("Hello {{ name }}!", { name = "World" })
-- "Hello World!"
```

## Performance

Templates are compiled to native Lua/JS source on first use, then cached. Subsequent renders reuse the compiled function. Call `template.clear_cache()` only if templates change at runtime (dev mode handles this automatically).
