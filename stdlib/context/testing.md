<!-- minimal -->
## Testing

Test files live alongside app code. Run with `hull test`.

```lua
-- Lua: tests/test_users.lua
local test = require("hull.test")

test("GET / returns 200", function()
    local res = test.get("/")
    test.eq(res.status, 200)
end)

test("POST /users creates user", function()
    local res = test.post("/users", { name = "Alice" })
    test.eq(res.status, 201)
    test.ok(res.body.ok)
end)
```

```javascript
// JS: tests/test_users.js
import { test } from "hull:test";

test("GET / returns 200", () => {
    const res = test.get("/");
    test.eq(res.status, 200);
});

test("POST /users creates user", () => {
    const res = test.post("/users", { name: "Alice" });
    test.eq(res.status, 201);
    test.ok(res.body.ok);
});
```

```bash
hull test           # run all tests
hull agent test     # JSON output for AI agents
```

<!-- compact -->
## Test HTTP Methods

- **`test.get(path, opts?)`** - GET request
- **`test.post(path, body?, opts?)`** - POST with JSON body
- **`test.put(path, body?, opts?)`** - PUT with JSON body
- **`test.delete(path, opts?)`** - DELETE request

`opts` can include `headers` table for custom headers:
```lua
local res = test.get("/api/data", { headers = { ["Authorization"] = "Bearer " .. token } })
```

## Response Object

- `res.status` - HTTP status code (number)
- `res.body` - parsed response body (table for JSON, string otherwise)
- `res.headers` - response headers table

## Assertions

- **`test.eq(actual, expected)`** - assert equality
- **`test.ok(value)`** - assert truthy
- **`test.err(fn, pattern?)`** - assert function throws (optional message pattern)

## Database in Tests

Tests run against an in-memory SQLite database. Migrations are applied automatically. Each test file gets a fresh database.

```lua
test("query returns inserted row", function()
    db.exec("INSERT INTO users (name) VALUES (?)", "Bob")
    local rows = db.query("SELECT * FROM users WHERE name = ?", "Bob")
    test.eq(#rows, 1)
    test.eq(rows[1].name, "Bob")
end)
```

## Agent Test Output

`hull agent test` produces JSON for programmatic consumption:

```bash
hull agent test myapp/
# { "tests": [...], "passed": 5, "failed": 0, "total": 5 }
```

<!-- full -->
## Complete Test File Example

```lua
local test = require("hull.test")

-- Setup: migrations run automatically, but you can seed data
test("list users empty", function()
    local res = test.get("/api/users")
    test.eq(res.status, 200)
    test.eq(#res.body, 0)
end)

test("create and retrieve user", function()
    local create = test.post("/api/users", { name = "Alice", email = "alice@test.com" })
    test.eq(create.status, 201)

    local list = test.get("/api/users")
    test.eq(#list.body, 1)
    test.eq(list.body[1].name, "Alice")
end)

test("duplicate email returns 409", function()
    test.post("/api/users", { name = "Bob", email = "bob@test.com" })
    local res = test.post("/api/users", { name = "Bob2", email = "bob@test.com" })
    test.eq(res.status, 409)
end)

test("delete user", function()
    local create = test.post("/api/users", { name = "Charlie", email = "charlie@test.com" })
    local id = create.body.id
    local res = test.delete("/api/users/" .. id)
    test.eq(res.status, 200)
end)
```

```javascript
import { test } from "hull:test";

test("create and retrieve user", () => {
    const create = test.post("/api/users", { name: "Alice", email: "alice@test.com" });
    test.eq(create.status, 201);

    const list = test.get("/api/users");
    test.eq(list.body.length, 1);
    test.eq(list.body[0].name, "Alice");
});
```

## Testing with Authentication

```lua
test("protected route requires auth", function()
    local res = test.get("/api/profile")
    test.eq(res.status, 401)
end)

test("protected route with JWT", function()
    local jwt = require("hull.jwt")
    local token = jwt.sign({ user_id = 1 }, "test-secret")
    local res = test.get("/api/profile", {
        headers = { ["Authorization"] = "Bearer " .. token }
    })
    test.eq(res.status, 200)
end)
```

## Running Tests

```bash
hull test                    # run all test files
hull test tests/test_api.lua # run specific file
hull agent test              # JSON output for CI/agents
```

Tests use in-process HTTP dispatch (no network). Requests go directly through the routing and middleware stack. This makes tests fast and deterministic.
