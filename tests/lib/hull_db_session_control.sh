# shellcheck shell=sh
# hull_db_session_control.sh - the session-control apps shared by e2e_mysql.sh
# and e2e_postgres.sh (audit convergence phase 3, docs/design/
# audit_convergence_sql_session.md).
#
# Hull no longer infers server session state from SQL text. Instead:
#   - app SQL that would change the shared session beyond the statement
#     (SET autocommit, LOCK TABLES, XA, GET_LOCK, FLUSH ... WITH READ LOCK,
#     backup locks; pg_advisory_lock, SET ROLE / SESSION AUTHORIZATION /
#     SESSION CHARACTERISTICS, LISTEN, ...) is refused with a clear error;
#   - every connection an entry used is reset at the entry boundary
#     (COM_RESET_CONNECTION / DISCARD ALL), so nothing it left on the session
#     reaches the next entry;
#   - a databases.named connection declared session_control = true takes
#     those statements, and the next entry on it still starts clean;
#   - Hull's own migration lock (GET_LOCK / pg_advisory_lock) still works,
#     while a migration FILE using one is refused.
#
# db_session_control_apps DIR DIALECT  writes DIR/l/app.lua and DIR/j/app.js
#                                      (each with migrations/), and
#                                      DIR/bad/app.lua whose migration uses a
#                                      named / advisory lock (DIALECT: mysql |
#                                      postgres). The apps read the
#                                      session_control connection's DSN from
#                                      HULL_SC_DSN.
# db_session_control_run PORT LEG...   GET /sc/LEG for each LEG; prints one
#                                      line per LEG:  SC LEG {...json...}
# db_session_control_ok OUT LEG WANT.. the LEG line of OUT holds every WANT
#                                      substring (::error and 1 otherwise)

db_session_control_apps() {
    mkdir -p "$1/l/migrations" "$1/j/migrations" "$1/bad/migrations"
    for _d in l j; do
        printf 'CREATE TABLE IF NOT EXISTS sc_mig (v INT);\n' \
            > "$1/$_d/migrations/001_sc.sql"
    done
    if [ "$2" = mysql ]; then
        printf "SELECT GET_LOCK('sc_bad', 0);\n" > "$1/bad/migrations/001_bad.sql"
    else
        printf 'SELECT pg_advisory_lock(4343);\n' > "$1/bad/migrations/001_bad.sql"
    fi
    cat > "$1/bad/app.lua" <<'LUA'
app.manifest({ modules = { "hull/db@1", "hull/http-server@1" } })
app.get("/", function(req, res) res:json({ up = true }) end)
LUA

    cat > "$1/l/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1", "hull/http-server@1" },
    secrets = { "HULL_SC_DSN" },
    databases = { named = {
        locks = { dsn = "$HULL_SC_DSN", session_control = true },
    } },
})
local db = require("hull.db")
local DIALECT = "@DIALECT@"

local REFUSED = DIALECT == "mysql" and {
    "SET autocommit = 0", "SET @@session.autocommit = 0",
    "SET @x = 1, autocommit = 0", "LOCK TABLES sc WRITE", "UNLOCK TABLES",
    "LOCK INSTANCE FOR BACKUP", "UNLOCK INSTANCE", "XA START 'sc'",
    "XA RECOVER", "SELECT GET_LOCK('sc', 0)", "SELECT RELEASE_LOCK('sc')",
    "SELECT RELEASE_ALL_LOCKS()", "FLUSH TABLES WITH READ LOCK",
    "FLUSH TABLES sc FOR EXPORT", "BACKUP LOCK sc", "BACKUP STAGE START",
    "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE",
    "SET NAMES latin1", "SELECT 1; SET autocommit = 0",
    "/*!50000 SET autocommit = 0 */",
} or {
    "SELECT pg_advisory_lock(1)", "SELECT pg_try_advisory_lock(1)",
    "SELECT pg_advisory_unlock_all()", "SET ROLE postgres", "RESET ROLE",
    "SET SESSION AUTHORIZATION DEFAULT",
    "SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY", "LISTEN sc",
    "UNLISTEN *", "PREPARE TRANSACTION 'sc'",
    "SELECT set_config('role', 'x', false)",
    "DO $$ BEGIN PERFORM pg_advisory_lock(1); END $$",
}

local function count(c, where)
    return tonumber(c.query("SELECT COUNT(*) AS n FROM sc " .. (where or ""))[1].n)
end

local leg = {}

-- Every session-altering statement fails, naming the escape hatch.
leg.refused = function(c)
    local n, bad, msg = 0, nil, nil
    for _, s in ipairs(REFUSED) do
        local ok, err = pcall(c.exec, s)
        if not ok and tostring(err):find("is refused", 1, true)
           and tostring(err):find("session_control", 1, true) then
            n = n + 1
            msg = msg or tostring(err):sub(1, 200)
        elseif not bad then
            bad = s .. " -> " .. tostring(ok and "allowed" or err):sub(1, 120)
        end
    end
    return { total = #REFUSED, refused = n, bad = bad or "none", msg = msg }
end

-- Plain transactions and db.batch still work.
leg.txn = function(c)
    c.exec("DELETE FROM sc")
    c.exec("BEGIN")
    c.exec("INSERT INTO sc VALUES (1)")
    c.exec("COMMIT")
    c.batch(function() c.exec("INSERT INTO sc VALUES (2)") end)
    c.exec(DIALECT == "mysql" and "START TRANSACTION" or "BEGIN")
    c.exec("INSERT INTO sc VALUES (3)")
    c.exec("ROLLBACK")
    return { rows = count(c), in_txn = c.in_transaction() }
end

-- Session state set inside an entry is visible in it, and gone in the next.
local function read_var(c)
    if DIALECT == "mysql" then
        return c.query("SELECT @hull_sc AS v")[1].v
    end
    return c.query("SELECT current_setting('application_name') AS v")[1].v
end
leg.var_set = function(c)
    c.exec(DIALECT == "mysql" and "SET @hull_sc = 42"
                              or "SET application_name = 'hull_sc'")
    return { v = tostring(read_var(c)) }
end
leg.var_get = function(c)
    local v = read_var(c)
    return { clear = (v == nil or v == "") }
end

-- A session_control connection takes the statements ...
leg.sc_hold = function(c)
    local l = db.connect("locks")
    if DIALECT == "mysql" then
        l.exec("LOCK TABLES sc WRITE")
        l.exec("INSERT INTO sc VALUES (5)")
        local other = pcall(l.query, "SELECT COUNT(*) AS n FROM sc2")
        l.exec("SET autocommit = 0")
        local ac = tonumber(l.query("SELECT @@autocommit AS a")[1].a)
        local g = tonumber(l.query("SELECT GET_LOCK('hull_sc', 0) AS g")[1].g)
        return { locked = not other, ac = ac, got_lock = g }
    end
    local g = l.query("SELECT pg_advisory_lock(4242) AS g")
    l.exec("SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY")
    local ro = l.query("SHOW default_transaction_read_only")[1]
    return { got_lock = g ~= nil, ro = ro.default_transaction_read_only }
end
-- ... and the next entry on it starts clean.
leg.sc_check = function(c)
    local l = db.connect("locks")
    if DIALECT == "mysql" then
        local ac = tonumber(l.query("SELECT @@autocommit AS a")[1].a)
        local other = pcall(l.query, "SELECT COUNT(*) AS n FROM sc2")
        local free = tonumber(l.query("SELECT IS_FREE_LOCK('hull_sc') AS f")[1].f)
        local w = pcall(c.exec, "INSERT INTO sc VALUES (6)")
        return { ac = ac, other_ok = other, lock_free = free, default_write = w }
    end
    local ro = l.query("SHOW default_transaction_read_only")[1]
    local held = tonumber(c.query(
        "SELECT COUNT(*) AS n FROM pg_locks WHERE locktype = 'advisory' " ..
        "AND objid = 4242")[1].n)
    local w = pcall(l.exec, "INSERT INTO sc VALUES (6)")
    return { ro = ro.default_transaction_read_only, held = held, sc_write = w }
end

app.get("/", function(req, res) res:json({ up = true }) end)   -- readiness

app.get("/setup", function(req, res)
    local c = db.default()
    c.exec("DROP TABLE IF EXISTS sc")
    c.exec("DROP TABLE IF EXISTS sc2")
    c.exec("CREATE TABLE sc (v INT)")
    c.exec("CREATE TABLE sc2 (v INT)")
    local mig = tonumber(c.query("SELECT COUNT(*) AS n FROM sc_mig")[1].n)
    res:json({ ok = true, migrated = mig == 0 })
end)

app.get("/sc/:leg", function(req, res)
    local f = leg[req.params.leg]
    if not f then res:status(404); res:json({ unknown = true }); return end
    res:json(f(db.default()))
end)
LUA

    cat > "$1/j/app.js" <<'JS'
import { app } from "hull:app";
import { db } from "hull:db";
app.manifest({
    modules: ["hull/db@1", "hull/http-server@1"],
    secrets: ["HULL_SC_DSN"],
    databases: { named: {
        locks: { dsn: "$HULL_SC_DSN", sessionControl: true },
    } },
});
const DIALECT = "@DIALECT@";

const REFUSED = DIALECT === "mysql" ? [
    "SET autocommit = 0", "SET @@session.autocommit = 0",
    "SET @x = 1, autocommit = 0", "LOCK TABLES sc WRITE", "UNLOCK TABLES",
    "LOCK INSTANCE FOR BACKUP", "UNLOCK INSTANCE", "XA START 'sc'",
    "XA RECOVER", "SELECT GET_LOCK('sc', 0)", "SELECT RELEASE_LOCK('sc')",
    "SELECT RELEASE_ALL_LOCKS()", "FLUSH TABLES WITH READ LOCK",
    "FLUSH TABLES sc FOR EXPORT", "BACKUP LOCK sc", "BACKUP STAGE START",
    "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE",
    "SET NAMES latin1", "SELECT 1; SET autocommit = 0",
    "/*!50000 SET autocommit = 0 */",
] : [
    "SELECT pg_advisory_lock(1)", "SELECT pg_try_advisory_lock(1)",
    "SELECT pg_advisory_unlock_all()", "SET ROLE postgres", "RESET ROLE",
    "SET SESSION AUTHORIZATION DEFAULT",
    "SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY", "LISTEN sc",
    "UNLISTEN *", "PREPARE TRANSACTION 'sc'",
    "SELECT set_config('role', 'x', false)",
    "DO $$ BEGIN PERFORM pg_advisory_lock(1); END $$",
];

const count = (c) => Number(c.query("SELECT COUNT(*) AS n FROM sc")[0].n);
const ok = (f) => { try { f(); return true; } catch (e) { return false; } };

const leg = {};

leg.refused = (c) => {
    let n = 0, bad = "none", msg = null;
    for (const s of REFUSED) {
        try {
            c.exec(s);
            if (bad === "none") bad = s + " -> allowed";
        } catch (e) {
            const m = String(e && e.message || e);
            if (m.includes("is refused") && m.includes("session_control")) {
                n++;
                if (!msg) msg = m.slice(0, 200);
            } else if (bad === "none") {
                bad = s + " -> " + m.slice(0, 120);
            }
        }
    }
    return { total: REFUSED.length, refused: n, bad, msg };
};

leg.txn = (c) => {
    c.exec("DELETE FROM sc");
    c.exec("BEGIN");
    c.exec("INSERT INTO sc VALUES (1)");
    c.exec("COMMIT");
    c.batch(() => { c.exec("INSERT INTO sc VALUES (2)"); });
    c.exec(DIALECT === "mysql" ? "START TRANSACTION" : "BEGIN");
    c.exec("INSERT INTO sc VALUES (3)");
    c.exec("ROLLBACK");
    return { rows: count(c), in_txn: c.inTransaction() };
};

const readVar = (c) => DIALECT === "mysql"
    ? c.query("SELECT @hull_sc AS v")[0].v
    : c.query("SELECT current_setting('application_name') AS v")[0].v;
leg.var_set = (c) => {
    c.exec(DIALECT === "mysql" ? "SET @hull_sc = 42"
                               : "SET application_name = 'hull_sc'");
    return { v: String(readVar(c)) };
};
leg.var_get = (c) => {
    const v = readVar(c);
    return { clear: v === null || v === undefined || v === "" };
};

leg.sc_hold = (c) => {
    const l = db.connect("locks");
    if (DIALECT === "mysql") {
        l.exec("LOCK TABLES sc WRITE");
        l.exec("INSERT INTO sc VALUES (5)");
        const other = ok(() => l.query("SELECT COUNT(*) AS n FROM sc2"));
        l.exec("SET autocommit = 0");
        const ac = Number(l.query("SELECT @@autocommit AS a")[0].a);
        const g = Number(l.query("SELECT GET_LOCK('hull_sc', 0) AS g")[0].g);
        return { locked: !other, ac, got_lock: g };
    }
    const g = l.query("SELECT pg_advisory_lock(4242) AS g");
    l.exec("SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY");
    const ro = l.query("SHOW default_transaction_read_only")[0];
    return { got_lock: g.length === 1, ro: ro.default_transaction_read_only };
};
leg.sc_check = (c) => {
    const l = db.connect("locks");
    if (DIALECT === "mysql") {
        const ac = Number(l.query("SELECT @@autocommit AS a")[0].a);
        const other = ok(() => l.query("SELECT COUNT(*) AS n FROM sc2"));
        const free = Number(l.query("SELECT IS_FREE_LOCK('hull_sc') AS f")[0].f);
        const w = ok(() => c.exec("INSERT INTO sc VALUES (6)"));
        return { ac, other_ok: other, lock_free: free, default_write: w };
    }
    const ro = l.query("SHOW default_transaction_read_only")[0];
    const held = Number(c.query(
        "SELECT COUNT(*) AS n FROM pg_locks WHERE locktype = 'advisory' " +
        "AND objid = 4242")[0].n);
    const w = ok(() => l.exec("INSERT INTO sc VALUES (6)"));
    return { ro: ro.default_transaction_read_only, held, sc_write: w };
};

app.get("/", (req, res) => res.json({ up: true }));

app.get("/setup", (req, res) => {
    const c = db.default();
    c.exec("DROP TABLE IF EXISTS sc");
    c.exec("DROP TABLE IF EXISTS sc2");
    c.exec("CREATE TABLE sc (v INT)");
    c.exec("CREATE TABLE sc2 (v INT)");
    const mig = Number(c.query("SELECT COUNT(*) AS n FROM sc_mig")[0].n);
    res.json({ ok: true, migrated: mig === 0 });
});

app.get("/sc/:leg", (req, res) => {
    const f = leg[req.params.leg];
    if (!f) { res.status(404); res.json({ unknown: true }); return; }
    res.json(f(db.default()));
});
JS
    for _f in "$1/l/app.lua" "$1/j/app.js"; do
        sed "s/@DIALECT@/$2/" "$_f" > "$_f.tmp" && mv "$_f.tmp" "$_f"
    done
}

db_session_control_run() {
    _port="$1"; shift
    for _leg in "$@"; do
        echo "SC ${_leg} $(curl -s "http://127.0.0.1:${_port}/sc/${_leg}")"
    done
}

db_session_control_ok() {
    _out="$1"; _leg="$2"; shift 2
    _line=$(echo "$_out" | grep "^SC $_leg " || true)
    for _want in "$@"; do
        case "$_line" in
            *"$_want"*) ;;
            *) echo "::error session control ($_leg): want $_want in: $_line"
               return 1 ;;
        esac
    done
    return 0
}
