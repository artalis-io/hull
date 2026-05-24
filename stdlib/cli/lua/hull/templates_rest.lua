--
-- hull.templates_rest — `hull new --type rest <name>` scaffolding
--
-- Returns a table { ["relative/path"] = "content" } for the modular
-- REST API layout. Mirrors the convention documented in CLAUDE.md
-- §App Layout Conventions:
--
--   app.lua           — manifest + bootstrap: requires route groups
--   routes/           — one file per resource, exports register(app)
--   middleware/       — app-specific middleware (wraps stdlib)
--   models/           — DB access functions per resource
--   lib/              — shared helpers
--   migrations/       — SQL schema migrations
--   tests/            — mirrors source tree (tests/routes/users_test.lua)
--
-- The Lua and JS surface stay in lock-step: same files, same shape,
-- same routes. JS uses ES-module export { register } instead of
-- Lua's `M.register`, but the bootstrap idiom in app.{lua,js} is
-- identical.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- ── Lua templates ────────────────────────────────────────────────────

local lua_files = {}

lua_files["app.lua"] = [[-- Modular REST API scaffold.
--
-- This file is just the bootstrap: declare the manifest, initialise
-- session/middleware modules that need it, and mount each route group.
-- Add a new resource by creating routes/<name>.lua + models/<name>.lua
-- and requiring its `register(app)` here.

-- Declare every first-party module the app + subfiles import. The
-- runtime tracker validates this list against actual imports at load
-- time, so missing entries surface immediately. `hull/middleware/session`
-- and friends auto-pull their own deps (json, time, crypto, db) via
-- the registry's deps graph — but anything imported only by user files
-- (routes/, models/, lib/) must be listed here explicitly.
app.manifest({
    modules = {
        "hull/http-server@1",
        "hull/middleware/logger@1",
        "hull/log@1",
        "hull/db@1",           -- models/user.lua
        "hull/crypto@1",       -- models/user.lua (random id)
        "hull/time@1",         -- models/user.lua (created_at)
        "hull/validate@1",     -- lib/validate_user.lua
        "hull/json@1",         -- routes/users.lua (request body decode)
    },
})

local log    = require("hull.log")
local logger = require("hull.middleware.logger")

-- Cross-cutting middleware first: request logging on every path.
app.use("*", "/*", logger.middleware({}))

-- Mount each route group. Add new groups below as the app grows.
require("./routes/health").register(app)
require("./routes/users").register(app)

log.info("rest api loaded")
]]

lua_files["routes/health.lua"] = [[-- routes/health.lua — liveness + readiness endpoints.
--
-- The health endpoint is intentionally trivial and unauthenticated:
-- a loadbalancer or container orchestrator hits it to decide whether
-- to route traffic to this process. Anything heavier (DB ping etc.)
-- belongs in /ready, which can use the stdlib `hull.middleware.health`
-- module when you're ready to wire it up.

local M = {}

function M.register(app)
    app.get("/health", function(_req, res)
        res:json({ status = "ok" })
    end)
end

return M
]]

lua_files["routes/users.lua"] = [[-- routes/users.lua — User resource.
--
-- Route registration only. The actual DB access lives in models/user.lua
-- so the routes stay readable and the model can be unit-tested in
-- isolation. The validate module wraps `hull.validate` with the
-- per-resource schema.

local json     = require("hull.json")
local users    = require("./../models/user")
local validate = require("./../lib/validate_user")

-- Tiny inline helper: req.body is the raw request bytes, not a parsed
-- JSON object. A real app would put this in lib/req.lua and reuse it
-- across resources. Returns (table, nil) on success or (nil, err).
local function parse_json_body(req)
    if not req.body or req.body == "" then return {}, nil end
    local ok, decoded = pcall(json.decode, req.body)
    if not ok then return nil, "invalid json" end
    return decoded, nil
end

local M = {}

function M.register(app)
    -- GET /users — list (paginated via ?limit=)
    app.get("/users", function(req, res)
        local limit = tonumber(req.query.limit) or 50
        res:json({ users = users.list({ limit = limit }) })
    end)

    -- POST /users — create
    app.post("/users", function(req, res)
        local body, perr = parse_json_body(req)
        if perr then res:status(400):json({ error = perr }); return end

        local ok, errors = validate.create(body)
        if not ok then
            res:status(400):json({ errors = errors })
            return
        end
        local u, err = users.create(body)
        if err then
            res:status(500):json({ error = err })
            return
        end
        res:status(201):json(u)
    end)

    -- GET /users/:id
    app.get("/users/:id", function(req, res)
        local u = users.find_by_id(req.params.id)
        if not u then
            res:status(404):json({ error = "not_found" })
            return
        end
        res:json(u)
    end)

    -- DELETE /users/:id
    app.delete("/users/:id", function(req, res)
        local removed = users.delete_by_id(req.params.id)
        if not removed then
            res:status(404):json({ error = "not_found" })
            return
        end
        res:status(204)
    end)
end

return M
]]

lua_files["models/user.lua"] = [[-- models/user.lua — Persistence layer for the User resource.
--
-- One file per resource keeps SQL in one place and lets routes/users.lua
-- stay focused on HTTP concerns. The returned table is the resource's
-- public surface; route handlers shouldn't touch `db` directly.

local db     = require("hull.db")
local crypto = require("hull.crypto")
local time   = require("hull.time")

local M = {}

function M.create(input)
    -- 32 random bytes → SHA-256 hex (64 chars). crypto.random returns
    -- raw bytes; sha256 returns a hex string. Combined this gives a
    -- cryptographically-strong opaque id with no separate hex-encoder.
    local id  = crypto.sha256(crypto.random(32))
    local now = time.now()
    db.exec(
        "INSERT INTO users (id, email, name, created_at) VALUES (?, ?, ?, ?)",
        { id, input.email, input.name, now }
    )
    return M.find_by_id(id)
end

function M.find_by_id(id)
    local rows = db.query(
        "SELECT id, email, name, created_at FROM users WHERE id = ?",
        { id }
    )
    return rows[1]
end

function M.list(opts)
    opts = opts or {}
    local limit = opts.limit or 50
    return db.query(
        "SELECT id, email, name, created_at FROM users " ..
        "ORDER BY created_at DESC LIMIT ?",
        { limit }
    )
end

function M.delete_by_id(id)
    local rows = db.query("SELECT id FROM users WHERE id = ?", { id })
    if #rows == 0 then return false end
    db.exec("DELETE FROM users WHERE id = ?", { id })
    return true
end

return M
]]

lua_files["lib/validate_user.lua"] = [[-- lib/validate_user.lua — Schema validation for the User resource.
--
-- Wraps `hull.validate` with per-resource schemas so routes/users.lua
-- can call `validate.create(body)` instead of inlining the rule table.
-- Adding a new field is a one-line schema change here.

local validate = require("hull.validate")

local M = {}

function M.create(body)
    return validate.check(body, {
        email = { required = true, type = "string", email = true, max = 254 },
        name  = { required = true, type = "string", trim = true, min = 1, max = 100 },
    })
end

return M
]]

lua_files["middleware/require_auth.lua"] = [[-- middleware/require_auth.lua — App-specific auth wrapper.
--
-- This is where app-specific authentication policy lives: the stdlib
-- module `hull.middleware.auth` provides the session-cookie / JWT
-- primitives; this wrapper composes them with app conventions like
-- "redirect to /login instead of 401" or "always require a verified
-- email."
--
-- Empty by default — uncomment and adapt when you add login.

-- local auth = require("hull.middleware.auth")
--
-- local M = {}
--
-- function M.require_user()
--     return auth.session_middleware({
--         login_path = "/login",
--     })
-- end
--
-- return M

return {}
]]

lua_files["migrations/001_init.sql"] = [[-- Initial schema for the modular REST scaffold.
CREATE TABLE IF NOT EXISTS users (
    id          TEXT PRIMARY KEY,
    email       TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    created_at  INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_users_created_at ON users(created_at);
]]

-- Hull's test runner discovers files matching `test_*.{lua,js}` recursively.
-- The mirror-source-tree convention (tests/routes/test_users.lua) keeps
-- per-resource tests next to where they conceptually belong.
lua_files["tests/routes/test_health.lua"] = [[test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)
]]

lua_files["tests/routes/test_users.lua"] = [[-- Hull's test.post takes an opts table where `body` is a raw string;
-- routes/users.lua decodes JSON manually, so the tests json-encode here.
local json = require("hull.json")

test("POST /users creates a user", function()
    local res = test.post("/users", {
        body = json.encode({ email = "a@b.test", name = "Alice" }),
    })
    test.eq(res.status, 201)
    test.ok(res.json.id)
    test.eq(res.json.email, "a@b.test")
end)

test("POST /users rejects missing email", function()
    local res = test.post("/users", {
        body = json.encode({ name = "no email" }),
    })
    test.eq(res.status, 400)
    test.ok(res.json.errors.email)
end)

test("GET /users/:id returns 404 for unknown id", function()
    local res = test.get("/users/does-not-exist")
    test.eq(res.status, 404)
end)
]]

-- ── JS templates (parity with Lua) ───────────────────────────────────

local js_files = {}

js_files["app.js"] = [[// Modular REST API scaffold.
//
// This file is just the bootstrap: declare the manifest, initialise
// session/middleware modules that need it, and mount each route group.
// Add a new resource by creating routes/<name>.js + models/<name>.js
// and importing its `register` here.

import { app }    from "hull:app";
import { log }    from "hull:log";
import { logger } from "hull:middleware:logger";

import { register as registerHealth } from "./routes/health.js";
import { register as registerUsers }  from "./routes/users.js";

// Declare every first-party module the app + subfiles import. The
// runtime tracker validates this list against actual imports at load
// time, so missing entries surface immediately.
app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/middleware/logger@1",
        "hull/log@1",
        "hull/db@1",           // models/user.js
        "hull/crypto@1",       // models/user.js (random id)
        "hull/time@1",         // models/user.js (created_at)
        "hull/validate@1",     // lib/validate_user.js
    ],
});

// Cross-cutting middleware first: request logging on every path.
app.use("*", "/*", logger.middleware({}));

// Mount each route group. Add new groups below as the app grows.
registerHealth(app);
registerUsers(app);

log.info("rest api loaded");
]]

js_files["routes/health.js"] = [[// routes/health.js — liveness + readiness endpoints.

export function register(app) {
    app.get("/health", (_req, res) => {
        res.json({ status: "ok" });
    });
}
]]

js_files["routes/users.js"] = [[// routes/users.js — User resource.

import * as users      from "./../models/user.js";
import * as userSchema from "./../lib/validate_user.js";

// req.body is the raw request bytes, not a parsed object — Hull's JS
// bindings expose the body as a string. Each route that consumes JSON
// decodes it here. A real app would put this in lib/req.js and reuse
// it across resources.
function parseJsonBody(req) {
    if (!req.body) return [{}, null];
    try {
        return [JSON.parse(req.body), null];
    } catch (_e) {
        return [null, "invalid json"];
    }
}

export function register(app) {
    // GET /users — list (paginated via ?limit=)
    app.get("/users", (req, res) => {
        const limit = parseInt(req.query.limit, 10) || 50;
        res.json({ users: users.list({ limit }) });
    });

    // POST /users — create
    app.post("/users", (req, res) => {
        const [body, perr] = parseJsonBody(req);
        if (perr) { res.status(400).json({ error: perr }); return; }

        const [ok, errors] = userSchema.create(body);
        if (!ok) {
            res.status(400).json({ errors });
            return;
        }
        const [u, err] = users.create(body);
        if (err) {
            res.status(500).json({ error: err });
            return;
        }
        res.status(201).json(u);
    });

    // GET /users/:id
    app.get("/users/:id", (req, res) => {
        const u = users.findById(req.params.id);
        if (!u) {
            res.status(404).json({ error: "not_found" });
            return;
        }
        res.json(u);
    });

    // DELETE /users/:id
    app.delete("/users/:id", (req, res) => {
        const removed = users.deleteById(req.params.id);
        if (!removed) {
            res.status(404).json({ error: "not_found" });
            return;
        }
        res.status(204);
    });
}
]]

js_files["models/user.js"] = [[// models/user.js — Persistence layer for the User resource.

import { db }     from "hull:db";
import { crypto } from "hull:crypto";
import { time }   from "hull:time";

export function create(input) {
    // Random bytes → SHA-256 hex (64 chars). crypto.random returns
    // raw bytes; sha256 returns a hex string. Combined this gives a
    // cryptographically-strong opaque id with no separate hex-encoder.
    const id  = crypto.sha256(crypto.random(32));
    const now = time.now();
    db.exec(
        "INSERT INTO users (id, email, name, created_at) VALUES (?, ?, ?, ?)",
        [id, input.email, input.name, now]
    );
    return [findById(id), null];
}

export function findById(id) {
    const rows = db.query(
        "SELECT id, email, name, created_at FROM users WHERE id = ?",
        [id]
    );
    return rows[0] ?? null;
}

export function list(opts) {
    const limit = (opts && opts.limit) || 50;
    return db.query(
        "SELECT id, email, name, created_at FROM users " +
        "ORDER BY created_at DESC LIMIT ?",
        [limit]
    );
}

export function deleteById(id) {
    const rows = db.query("SELECT id FROM users WHERE id = ?", [id]);
    if (rows.length === 0) return false;
    db.exec("DELETE FROM users WHERE id = ?", [id]);
    return true;
}
]]

js_files["lib/validate_user.js"] = [[// lib/validate_user.js — Schema validation for the User resource.

import { validate } from "hull:validate";

export function create(body) {
    return validate.check(body, {
        email: { required: true, type: "string", email: true, max: 254 },
        name:  { required: true, type: "string", trim: true, min: 1, max: 100 },
    });
}
]]

js_files["middleware/require_auth.js"] = [[// middleware/require_auth.js — App-specific auth wrapper.
//
// Empty by default — uncomment and adapt when you add login.

// import { auth } from "hull:middleware:auth";
//
// export function requireUser() {
//     return auth.sessionMiddleware({ loginPath: "/login" });
// }
]]

js_files["migrations/001_init.sql"] = lua_files["migrations/001_init.sql"]

js_files["tests/routes/test_health.js"] = [[test("GET /health returns ok", async () => {
    const res = await test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});
]]

js_files["tests/routes/test_users.js"] = [[// Hull's test.post takes an opts object where `body` is a raw string;
// routes/users.js decodes JSON manually, so the tests JSON-encode here.

test("POST /users creates a user", async () => {
    const res = await test.post("/users", {
        body: JSON.stringify({ email: "a@b.test", name: "Alice" }),
    });
    test.eq(res.status, 201);
    test.ok(res.json.id);
    test.eq(res.json.email, "a@b.test");
});

test("POST /users rejects missing email", async () => {
    const res = await test.post("/users", {
        body: JSON.stringify({ name: "no email" }),
    });
    test.eq(res.status, 400);
    test.ok(res.json.errors.email);
});

test("GET /users/:id returns 404 for unknown id", async () => {
    const res = await test.get("/users/does-not-exist");
    test.eq(res.status, 404);
});
]]

-- ── Shared template (runtime-agnostic) ───────────────────────────────

local gitignore = [[data.db
data.db-*
*.key
hull.sig
build/
]]

-- ── Public API ───────────────────────────────────────────────────────

function M.files(runtime)
    local out = (runtime == "js") and js_files or lua_files
    -- Shallow copy + add the runtime-agnostic gitignore.
    local copy = {}
    for k, v in pairs(out) do copy[k] = v end
    copy[".gitignore"] = gitignore
    return copy
end

return M
