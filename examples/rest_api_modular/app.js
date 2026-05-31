// Modular REST API scaffold.
//
// This file is just the bootstrap: declare the manifest, initialise
// session/middleware modules that need it, and mount each route group.
// Add a new resource by creating routes/<name>.js + models/<name>.js
// and importing its `register` here.

import { app }    from "hull:app";
import { log }    from "hull:log";
import { logger } from "hull:web:middleware:logger";

import { register as registerHealth } from "./routes/health.js";
import { register as registerUsers }  from "./routes/users.js";

// Declare every first-party module the app + subfiles import. The
// runtime tracker validates this list against actual imports at load
// time, so missing entries surface immediately.
app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/web/middleware/logger@1",
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
