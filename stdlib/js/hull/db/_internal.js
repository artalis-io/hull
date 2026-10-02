// hull:db:_internal - the connection the stdlib keeps its own _hull_* tables
// on (stdlib-only: the underscore segment keeps it out of app code).
//
// With `databases.internal` declared in the manifest it is that database,
// reached under a role the app's own connection has no grants on, so the
// separation is enforced by the database rather than by Hull's SQL-text
// check. Without it, the default connection, as before.
//
// A module takes its connection when it loads, which is before the manifest
// is wired, so this is a proxy: every access asks for the connection afresh
// until the manifest is wired, and caches it from then on. A method is handed
// back wrapped in a function defined HERE that calls it on the real
// connection: the _hull_* caller check needs `this` to be that connection and
// the calling frame to be stdlib (a bound function would be neither).

// The export is named for the module's last segment, like every hull:* native
// module: while `hull build` reads the manifest, this module is a stand-in that
// exports that name (and does nothing), so a stdlib init() at app top level
// runs through it.
import { _internal_conn as native } from "hull:db:_internal_conn";

let cached = null;

function resolve() {
    if (cached) return cached;
    const r = native.connection();
    if (r.final) cached = r.conn;
    return r.conn;
}

/** The internal connection, as a proxy usable at module load. */
function connection() {
    return new Proxy({}, {
        get(_, k) {
            const v = resolve()[k];
            if (typeof v === "function")
                return (...args) => resolve()[k](...args);
            return v;
        },
        set() { throw new TypeError("hull:db:_internal: read-only"); },
    });
}

export const internal = { connection };
