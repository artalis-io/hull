// routes/users.js - User resource.

import * as userSchema from "./../lib/validate_user.js";
import * as users      from "./../models/user.js";

// req.body is the raw request bytes, not a parsed object - Hull's JS
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
    // GET /users - list (paginated via ?limit=)
    app.get("/users", (req, res) => {
        const limit = Number.parseInt(req.query.limit, 10) || 50;
        res.json({ users: users.list({ limit }) });
    });

    // POST /users - create
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
