# shellcheck shell=sh
# hull_db_session_audit40.sh - the audit 40 apps shared by e2e_mysql.sh and
# e2e_postgres.sh. Round 39 resets a MySQL session left with autocommit off /
# LOCK TABLES / XA, and the stale-transaction guard also runs just before a
# parked handler resumes. Right after `SET autocommit = 0` or `LOCK TABLES`,
# with no statement after it, IN_TRANS is clear, so the wait gates let the
# handler park - and the guard before its resume reset the session under it:
# `SET autocommit = 0; <wait>; INSERT; ROLLBACK` committed the INSERT and the
# ROLLBACK lied; a LOCK TABLES was silently dropped mid-handler. That state
# now counts as an open transaction for the wait gates: the wait is refused
# (wait_refused) and nothing is reset under the handler.
#
# db_session_audit40_apps DIR DIALECT   writes DIR/l/app.lua and DIR/j/app.js
#                                       (DIALECT: mysql | postgres)
# db_session_audit40_run PORT KIND...   GET /a40/KIND for each KIND; prints
#                                       one line per KIND:
#                                         A40 KIND {...json...}
# db_session_audit40_ok OUT KIND WANT.. the KIND line of OUT holds every WANT
#                                       substring (::error and 1 otherwise)

db_session_audit40_apps() {
    mkdir -p "$1/l" "$1/j"
    cat > "$1/l/app.lua" <<'LUA'
app.manifest({ modules = { "hull/db@1", "hull/http-server@1" } })
local db = require("hull.db")
local DIALECT = "@DIALECT@"

-- pcall(hull.sleep): (true) or (false, the refusal code or "other")
local function try_wait()
    local ok, err = pcall(hull.sleep, 50)
    if ok then return true, nil end
    return false, tostring(err):find("[hull:wait_refused]", 1, true)
                  and "wait_refused" or tostring(err):sub(1, 120)
end

local function count(c)
    return tonumber(c.query("SELECT COUNT(*) AS n FROM a40")[1].n)
end

local leg = {}
if DIALECT == "mysql" then
    -- autocommit off, then a wait: refused, and the ROLLBACK still undoes
    -- the INSERT (it ran in the handler's own transaction).
    leg.autocommit_wait = function(c)
        c.exec("SET autocommit = 0")
        local waited, code = try_wait()
        c.exec("INSERT INTO a40 VALUES (1)")
        local in_txn = c.in_transaction()
        c.exec("ROLLBACK")
        c.exec("SET autocommit = 1")
        return { waited = waited, code = code, in_txn = in_txn, rows = count(c) }
    end
    -- LOCK TABLES, then a wait: refused, and the lock is still held (a
    -- table not named in it stays refused).
    leg.locktables_wait = function(c)
        c.exec("LOCK TABLES a40 WRITE")
        local waited, code = try_wait()
        local other = pcall(c.query, "SELECT COUNT(*) AS n FROM a40b")
        c.exec("UNLOCK TABLES")
        local after = pcall(hull.sleep, 1)
        return { waited = waited, code = code, still_locked = not other,
                 wait_after_unlock = after }
    end
    -- The autocommit-off idiom with no wait works as the app intends.
    leg.autocommit_nowait = function(c)
        c.exec("SET autocommit = 0")
        c.exec("INSERT INTO a40 VALUES (2)")
        c.exec("ROLLBACK")
        c.exec("INSERT INTO a40 VALUES (3)")
        c.exec("COMMIT")
        c.exec("SET autocommit = 1")
        local after = pcall(hull.sleep, 1)
        return { rows = count(c), wait_after_restore = after }
    end
else
    -- A session-level advisory lock does not count (documented): the wait
    -- goes ahead and the lock is still held after it - nothing released it
    -- under the handler.
    leg.advisory_wait = function(c)
        c.query("SELECT pg_advisory_lock(4040) AS g")
        local waited, code = try_wait()
        local held = tonumber(c.query(
            "SELECT COUNT(*) AS n FROM pg_locks WHERE locktype = 'advisory' " ..
            "AND objid = 4040 AND pid = pg_backend_pid()")[1].n)
        c.query("SELECT pg_advisory_unlock(4040) AS u")
        return { waited = waited, code = code, held_after_wait = held }
    end
    -- An open transaction still refuses the wait.
    leg.begin_wait = function(c)
        c.exec("BEGIN")
        local waited, code = try_wait()
        c.exec("INSERT INTO a40 VALUES (4)")
        c.exec("ROLLBACK")
        return { waited = waited, code = code, rows = count(c) }
    end
end

app.get("/", function(req, res) res:json({ up = true }) end)   -- readiness

app.get("/setup", function(req, res)
    local c = db.default()
    c.exec("DROP TABLE IF EXISTS a40")
    c.exec("DROP TABLE IF EXISTS a40b")
    c.exec("CREATE TABLE a40 (v INT)")
    c.exec("CREATE TABLE a40b (v INT)")
    res:json({ ok = true })
end)

app.get("/a40/:kind", function(req, res)
    local f = leg[req.params.kind]
    if not f then res:status(404); res:json({ unknown = true }); return end
    local c = db.default()
    c.exec("DELETE FROM a40")
    res:json(f(c))
end)
LUA
    cat > "$1/j/app.js" <<'JS'
import { app } from "hull:app";
import { db } from "hull:db";
app.manifest({ modules: ["hull/db@1", "hull/http-server@1"] });
const DIALECT = "@DIALECT@";

async function tryWait() {
    try {
        await hull.sleep(50);
        return [true, null];
    } catch (e) {
        return [false, e && e.code === "wait_refused" ? "wait_refused"
                                                      : String(e).slice(0, 120)];
    }
}

const count = (c) => Number(c.query("SELECT COUNT(*) AS n FROM a40")[0].n);

const leg = {};
if (DIALECT === "mysql") {
    leg.autocommit_wait = async (c) => {
        c.exec("SET autocommit = 0");
        const [waited, code] = await tryWait();
        c.exec("INSERT INTO a40 VALUES (1)");
        const in_txn = c.inTransaction();
        c.exec("ROLLBACK");
        c.exec("SET autocommit = 1");
        return { waited, code, in_txn, rows: count(c) };
    };
    leg.locktables_wait = async (c) => {
        c.exec("LOCK TABLES a40 WRITE");
        const [waited, code] = await tryWait();
        let other = true;
        try { c.query("SELECT COUNT(*) AS n FROM a40b"); } catch (e) { other = false; }
        c.exec("UNLOCK TABLES");
        const [after] = await tryWait();
        return { waited, code, still_locked: !other, wait_after_unlock: after };
    };
    leg.autocommit_nowait = async (c) => {
        c.exec("SET autocommit = 0");
        c.exec("INSERT INTO a40 VALUES (2)");
        c.exec("ROLLBACK");
        c.exec("INSERT INTO a40 VALUES (3)");
        c.exec("COMMIT");
        c.exec("SET autocommit = 1");
        const [after] = await tryWait();
        return { rows: count(c), wait_after_restore: after };
    };
} else {
    leg.advisory_wait = async (c) => {
        c.query("SELECT pg_advisory_lock(4040) AS g");
        const [waited, code] = await tryWait();
        const held = Number(c.query(
            "SELECT COUNT(*) AS n FROM pg_locks WHERE locktype = 'advisory' " +
            "AND objid = 4040 AND pid = pg_backend_pid()")[0].n);
        c.query("SELECT pg_advisory_unlock(4040) AS u");
        return { waited, code, held_after_wait: held };
    };
    leg.begin_wait = async (c) => {
        c.exec("BEGIN");
        const [waited, code] = await tryWait();
        c.exec("INSERT INTO a40 VALUES (4)");
        c.exec("ROLLBACK");
        return { waited, code, rows: count(c) };
    };
}

app.get("/", (req, res) => res.json({ up: true }));

app.get("/setup", (req, res) => {
    const c = db.default();
    c.exec("DROP TABLE IF EXISTS a40");
    c.exec("DROP TABLE IF EXISTS a40b");
    c.exec("CREATE TABLE a40 (v INT)");
    c.exec("CREATE TABLE a40b (v INT)");
    res.json({ ok: true });
});

app.get("/a40/:kind", async (req, res) => {
    const f = leg[req.params.kind];
    if (!f) { res.status(404); res.json({ unknown: true }); return; }
    const c = db.default();
    c.exec("DELETE FROM a40");
    res.json(await f(c));
});
JS
    for _f in "$1/l/app.lua" "$1/j/app.js"; do
        sed "s/@DIALECT@/$2/" "$_f" > "$_f.tmp" && mv "$_f.tmp" "$_f"
    done
}

db_session_audit40_run() {
    _port="$1"; shift
    for _kind in "$@"; do
        echo "A40 ${_kind} $(curl -s "http://127.0.0.1:${_port}/a40/${_kind}")"
    done
}

db_session_audit40_ok() {
    _out="$1"; _kind="$2"; shift 2
    _line=$(echo "$_out" | grep "^A40 $_kind " || true)
    for _want in "$@"; do
        case "$_line" in
            *"$_want"*) ;;
            *) echo "::error audit 40 ($_kind): want $_want in: $_line"
               return 1 ;;
        esac
    done
    return 0
}
