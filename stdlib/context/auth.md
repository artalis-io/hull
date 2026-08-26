<!-- minimal -->
## Authentication

**Session-based auth** — cookie sessions backed by SQLite.

```lua
-- Lua
local auth = require("hull.web.middleware.auth")
local session = require("hull.web.middleware.session")
session.init({ ttl = 86400 })

app.use("*", "/app/*", auth.session_middleware({ cookie_name = "hull_session" }))
auth.login(req, res, { user_id = 1 })   -- creates session, sets cookie
auth.logout(req, res)                    -- destroys session, clears cookie
```

```javascript
// JS
import { auth } from "hull:web:middleware:auth";
import { session } from "hull:web:middleware:session";
session.init({ ttl: 86400 });

app.use("*", "/app/*", auth.sessionMiddleware({ cookieName: "hull_session" }));
auth.login(req, res, { userId: 1 });
auth.logout(req, res);
```

**JWT auth** — Bearer token via `Authorization` header.

```lua
app.use("*", "/api/*", auth.jwt_middleware({ secret = "my-secret" }))
-- req.ctx.user contains decoded payload
```

<!-- compact -->
## Options

**`auth.session_middleware(opts)`** / `auth.sessionMiddleware(opts)`
- `cookie_name` / `cookieName` - session cookie name (default: `"hull_session"`)
- `optional` - if `true`, continues without session instead of rejecting (default: `false`)
- `login_path` / `loginPath` - redirect path on failure instead of 401
- Sets `req.ctx.session` (data) and `req.ctx.session_id` / `req.ctx.sessionId`

**`auth.jwt_middleware(opts)`** / `auth.jwtMiddleware(opts)`
- `secret` - HMAC-SHA256 secret (required)
- `optional` - continue without token (default: `false`)
- Reads `Authorization: Bearer <token>` header
- Sets `req.ctx.user` with decoded payload

**`auth.login(req, res, user_data, opts)`** — creates session, sets `Set-Cookie`. Returns `session_id`.

**`auth.logout(req, res, opts)`** — destroys session, clears cookie.

**Session management:**
```lua
local session = require("hull.web.middleware.session")
session.init({ ttl = 86400 })        -- call once at startup
session.create({ user_id = 1 })      -- returns 64-char hex ID
session.load(session_id)             -- returns data or nil, auto-extends expiry
session.update(session_id, data)     -- replace session data
session.destroy(session_id)          -- delete session
session.cleanup()                    -- delete expired sessions, returns count
```

**Middleware ordering:** Place CORS before auth so preflight requests don't require credentials. Place rate limiting before auth to reject abusive traffic early.

<!-- full -->
## Complete Login/Logout Flow

```lua
local auth    = require("hull.web.middleware.auth")
local session = require("hull.web.middleware.session")
local crypto  = require("hull.crypto")

session.init()

-- Protect all /app/* routes
app.use("*", "/app/*", auth.session_middleware({
    login_path = "/login"   -- redirect instead of 401
}))

app.post("/login", function(req, res)
    local body = req.body
    local rows = db.query("SELECT id, password_hash FROM users WHERE email = ?", body.email)
    if #rows == 0 then
        return res.status(401).json({ error = "Invalid credentials" })
    end
    local user = rows[1]
    if not crypto.pbkdf2_verify(body.password, user.password_hash) then
        return res.status(401).json({ error = "Invalid credentials" })
    end
    auth.login(req, res, { user_id = user.id })
    res.redirect("/app/dashboard")
end)

app.post("/logout", function(req, res)
    auth.logout(req, res)
    res.redirect("/login")
end)
```

## JWT API Pattern

```javascript
import { auth } from "hull:web:middleware:auth";
import { jwt } from "hull:jwt";

const SECRET = env.get("JWT_SECRET");

app.use("*", "/api/*", auth.jwtMiddleware({ secret: SECRET }));

app.post("/api/login", (req, res) => {
    // validate credentials...
    const token = jwt.sign({ userId: user.id, role: "admin" }, SECRET);
    res.json({ token });
});

app.get("/api/profile", (req, res) => {
    // req.ctx.user is the decoded JWT payload
    const rows = db.query("SELECT * FROM users WHERE id = ?", req.ctx.user.userId);
    res.json(rows[0]);
});
```

## Security Notes

- **CSRF is for cookies only.** Session auth needs CSRF middleware. JWT Bearer auth does not (browsers don't auto-attach Bearer tokens).
- **Session cleanup:** Call `session.cleanup()` periodically via `app.every()` to remove expired sessions.
- **JWT has no revocation.** Use short-lived tokens. For logout capability, track revoked tokens in the database.
- **`optional: true`** lets unauthenticated requests through -- use this for routes that behave differently for logged-in users but don't require auth.
