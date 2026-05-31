<!-- minimal -->
## Input Validation

Declarative schema validation and form body parsing.

```lua
-- Lua
local validate = require("hull.validate")

local ok, errors = validate.check(req.body, {
    name  = { required = true, type = "string", min = 1, max = 100 },
    email = { required = true, email = true },
    age   = { type = "integer", min = 0, max = 150 },
})
if not ok then
    return res.status(400).json({ errors = errors })
end
```

```javascript
// JS
import { validate } from "hull:validate";

const [ok, errors] = validate.check(req.body, {
    name:  { required: true, type: "string", min: 1, max: 100 },
    email: { required: true, email: true },
    age:   { type: "integer", min: 0, max: 150 },
});
if (!ok) return res.status(400).json({ errors });
```

**Form parsing:**
```lua
local form = require("hull.web.form")
local data = form.parse(req.body)  -- URL-encoded form -> table
```

<!-- compact -->
## Schema Rules

| Rule | Type | Description |
|------|------|-------------|
| `required` | boolean | Field must be present and non-empty |
| `trim` | boolean | Trim whitespace before validation |
| `type` | string | `"string"`, `"number"`, `"integer"`, `"boolean"` |
| `min` | number | Minimum string length or numeric value |
| `max` | number | Maximum string length or numeric value |
| `pattern` | string | Regex pattern the value must match |
| `oneof` | array | Value must be one of the listed options |
| `email` | boolean | Must be a valid email format |
| `fn` | function | Custom validator: `fn(value) -> true` or `fn(value) -> false, "message"` |
| `message` | string | Custom error message (overrides default) |

`min`/`max` check string length for string types, numeric value for number/integer types.

## Return Value

`validate.check(data, schema)` returns `(ok, errors)`:
- `ok` = `true` if all fields pass, `false` if any fail
- `errors` = table mapping field names to error strings: `{ email = "invalid email", age = "must be at least 0" }`

## Form Parsing

```lua
local form = require("hull.web.form")
-- Parses: "name=Alice&email=alice%40test.com&role=admin"
local data = form.parse(req.body)
-- data = { name = "Alice", email = "alice@test.com", role = "admin" }
```

- Decodes `+` as space, `%XX` percent-encoding
- Last value wins for duplicate keys
- Returns empty table for nil/empty input

<!-- full -->
## Complete Validation Example

```lua
local validate = require("hull.validate")
local form = require("hull.web.form")

app.post("/api/users", function(req, res)
    local ok, errors = validate.check(req.body, {
        name     = { required = true, trim = true, type = "string", min = 1, max = 200 },
        email    = { required = true, trim = true, email = true },
        password = { required = true, type = "string", min = 8, max = 128 },
        role     = { oneof = { "user", "admin" }, message = "role must be user or admin" },
        age      = { type = "integer", min = 13, max = 150 },
        website  = { type = "string", pattern = "^https?://" },
    })
    if not ok then
        return res.status(400).json({ errors = errors })
    end
    -- req.body fields are now validated
    db.exec("INSERT INTO users (name, email) VALUES (?, ?)", req.body.name, req.body.email)
    res.status(201).json({ ok = true })
end)
```

```javascript
import { validate } from "hull:validate";

app.post("/api/users", (req, res) => {
    const [ok, errors] = validate.check(req.body, {
        name:     { required: true, trim: true, type: "string", min: 1, max: 200 },
        email:    { required: true, trim: true, email: true },
        password: { required: true, type: "string", min: 8, max: 128 },
        role:     { oneof: ["user", "admin"], message: "role must be user or admin" },
        age:      { type: "integer", min: 13, max: 150 },
    });
    if (!ok) return res.status(400).json({ errors });
    db.exec("INSERT INTO users (name, email) VALUES (?, ?)", req.body.name, req.body.email);
    res.status(201).json({ ok: true });
});
```

## Custom Validator

```lua
local ok, errors = validate.check(req.body, {
    username = {
        required = true,
        fn = function(value)
            if value:match("^[a-zA-Z0-9_]+$") then
                return true
            end
            return false, "must contain only letters, numbers, and underscores"
        end,
    },
})
```

## HTML Form Handling

```lua
-- For <form method="POST" enctype="application/x-www-form-urlencoded">
app.post("/signup", function(req, res)
    local data = form.parse(req.body)
    local ok, errors = validate.check(data, {
        username = { required = true, trim = true, min = 3 },
        password = { required = true, min = 8 },
    })
    if not ok then
        return res.html(template.render("signup.html", { errors = errors }))
    end
    -- create user...
    res.redirect("/login")
end)
```

## Notes

- Fields not in the schema are ignored (pass-through).
- `required = true` fails on `nil`, empty string `""`, and whitespace-only strings (when `trim = true`).
- `trim = true` modifies the value in-place before other rules run.
- The `email` rule checks basic format (`x@y.z`), not deliverability.
- Use `pattern` for custom format validation (Lua patterns for Lua, regex for JS).
