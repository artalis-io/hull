// routes/users.js — User resource.

import * as users      from "./../models/user.js";
import * as userSchema from "./../lib/validate_user.js";

export function register(app) {
    // GET /users — list (paginated via ?limit=)
    app.get("/users", (req, res) => {
        const limit = parseInt(req.query.limit, 10) || 50;
        res.json({ users: users.list({ limit }) });
    });

    // POST /users — create
    app.post("/users", (req, res) => {
        const body = req.body || {};
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
