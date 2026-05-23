-- routes/users.lua — User resource.
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
