# shellcheck shell=sh
# hull_db_session_audit41.sh - the audit 41 apps for e2e_mysql.sh.
#
# A lost MySQL connection (server restart, KILL, read_timeout) was replaced
# by a fresh one only when its last status had IN_TRANS. After SET
# autocommit = 0, LOCK TABLES, or a COMMIT made with autocommit off, IN_TRANS
# is clear: the call that met the dead socket failed once, the next one
# silently reconnected into an autocommit session with no lock, and waits
# were allowed meanwhile. A retried INSERT autocommitted and the later
# ROLLBACK "succeeded" with the row left; other sessions read the "locked"
# table. Calls are now refused until the app ends that state (ROLLBACK, SET
# autocommit = 1, UNLOCK TABLES) or the entry ends, and waits stay refused.
#
# Also: a `--` followed by a form feed is a comment to MySQL (a release word
# behind it no longer releases a tracked LOCK TABLES), a server whose global
# autocommit is off no longer has every write rolled back by the stale
# guard, and a CALL that opened a transaction (or turned autocommit off)
# before it failed is seen (COM_PING after a failed statement).
#
# db_session_audit41_apps DIR DSN       writes DIR/l/app.lua and DIR/j/app.js;
#                                       DSN is also the "killer" connection
# db_session_audit41_run PORT KIND...   GET /a41/KIND for each KIND; prints
#                                       one line per KIND:
#                                         A41 KIND {...json...}
# db_session_audit41_ok OUT KIND WANT.. the KIND line of OUT holds every WANT
#                                       substring (::error and 1 otherwise)

db_session_audit41_apps() {
    mkdir -p "$1/l" "$1/j"
    cat > "$1/l/app.lua" <<'LUA'
app.manifest({ modules = { "hull/db@1", "hull/http-server@1" },
               databases = { named = { killer = "@DSN@" } } })
local db = require("hull.db")

local function try_wait()
    local ok, err = pcall(hull.sleep, 20)
    if ok then return true, nil end
    return false, tostring(err):find("[hull:wait_refused]", 1, true)
                  and "wait_refused" or tostring(err):sub(1, 120)
end

local function count(k)
    return tonumber(k.query("SELECT COUNT(*) AS n FROM a41")[1].n)
end

-- Kill the default connection's session from another one.
local function kill(c, k)
    local id = c.query("SELECT CONNECTION_ID() AS id")[1].id
    k.exec("KILL " .. string.format("%d", math.tointeger(id) or tonumber(id)))
end

-- Up to three writes after the kill: none may succeed, and one must be
-- refused naming @p want (the first may just meet the dead socket).
local function writes_after_kill(c, want)
    local ok_any, refused = false, false
    for i = 1, 3 do
        local ok, e = pcall(c.exec, "INSERT INTO a41 VALUES (" .. (10 + i) .. ")")
        if ok then ok_any = true end
        if not ok and tostring(e):find(want, 1, true) then refused = true end
    end
    return ok_any, refused
end

local leg = {}
leg.kill_autocommit = function(c, k)
    c.exec("SET autocommit = 0")
    kill(c, k)
    local wrote, refused = writes_after_kill(c, "autocommit off")
    local waited, code = try_wait()
    local rb = pcall(c.exec, "ROLLBACK")
    -- The next call reconnects, with autocommit off again: the app's
    -- session as it believes it is. Its write is undone by its ROLLBACK.
    local after = pcall(c.exec, "INSERT INTO a41 VALUES (20)")
    local ac = tonumber(c.query("SELECT @@session.autocommit AS a")[1].a)
    c.exec("ROLLBACK")
    c.exec("SET autocommit = 1")
    return { wrote = wrote, refused = refused, waited = waited, code = code,
             rollback_ok = rb, after_rollback_ok = after, ac_after = ac,
             rows = count(k) }
end
leg.kill_locktables = function(c, k)
    c.exec("LOCK TABLES a41 WRITE")
    kill(c, k)
    local wrote, refused = writes_after_kill(c, "LOCK TABLES")
    local waited, code = try_wait()
    local rb = pcall(c.exec, "ROLLBACK")      -- does not release a table lock
    local still = not pcall(c.query, "SELECT 1 AS x")
    local unlock = pcall(c.exec, "UNLOCK TABLES")
    local after = pcall(c.query, "SELECT COUNT(*) AS n FROM a41")
    local after_wait = pcall(hull.sleep, 1)
    return { wrote = wrote, refused = refused, waited = waited, code = code,
             rollback_ok = rb, still_refused = still, unlock_ok = unlock,
             after_unlock_ok = after, wait_after_unlock = after_wait,
             rows = count(k) }
end
-- Killed with autocommit off and never restored: the entry ends refused;
-- the next entry gets a fresh autocommit session (the guard forgot it).
leg.kill_and_leave = function(c, k)
    c.exec("SET autocommit = 0")
    kill(c, k)
    local wrote = writes_after_kill(c, "autocommit off")
    return { wrote = wrote }
end
leg.probe = function(c, k)
    local ac = tonumber(c.query("SELECT @@session.autocommit AS a")[1].a)
    local waited = try_wait()
    return { ac = ac, waited = waited, rows = count(k) }
end
leg.dash_comment = function(c, k)
    c.exec("LOCK TABLES a41 WRITE")
    c.query("SELECT 1 AS a --\fUNLOCK TABLES")
    local waited, code = try_wait()
    c.exec("UNLOCK TABLES")
    local after = pcall(hull.sleep, 1)
    return { waited = waited, code = code, wait_after_unlock = after }
end
leg.call_txn = function(c, k)
    local ok = pcall(c.exec, "CALL a41_txn()")
    local in_txn = c.in_transaction()
    local waited, code = try_wait()
    return { call_ok = ok, in_txn = in_txn, waited = waited, code = code }
end
leg.call_ac = function(c, k)
    local ok = pcall(c.exec, "CALL a41_ac()")
    local waited, code = try_wait()
    return { call_ok = ok, waited = waited, code = code }
end
leg.write60 = function(c, k)
    c.exec("INSERT INTO a41 VALUES (60)")
    return { ok = true }
end
leg.write61 = function(c, k)
    c.exec("INSERT INTO a41 VALUES (61)")
    return { ok = true }
end
-- Global autocommit off: a plain write persists (no explicit COMMIT).
leg.global_write = function(c, k)
    local ac = tonumber(c.query("SELECT @@session.autocommit AS a")[1].a)
    c.exec("INSERT INTO a41 VALUES (70)")
    return { ac = ac }
end
-- Leaves LOCK TABLES: the entry-end guard resets the session, and the reset
-- session must run with autocommit on too.
leg.global_lock = function(c, k)
    c.exec("LOCK TABLES a41 WRITE")
    return { ok = true }
end
leg.global_write2 = function(c, k)
    local ac = tonumber(c.query("SELECT @@session.autocommit AS a")[1].a)
    c.exec("INSERT INTO a41 VALUES (71)")
    return { ac = ac }
end

app.get("/", function(req, res) res:json({ up = true }) end)   -- readiness

app.get("/setup", function(req, res)
    local c = db.default()
    c.exec("DROP TABLE IF EXISTS a41")
    c.exec("CREATE TABLE a41 (v INT)")
    c.exec("DROP PROCEDURE IF EXISTS a41_txn")
    c.exec("DROP PROCEDURE IF EXISTS a41_ac")
    c.exec("CREATE PROCEDURE a41_txn() BEGIN START TRANSACTION; " ..
           "INSERT INTO a41 VALUES (50); " ..
           "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'a41 boom'; END")
    c.exec("CREATE PROCEDURE a41_ac() BEGIN SET autocommit = 0; " ..
           "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'a41 boom'; END")
    res:json({ ok = true })
end)

app.get("/reset", function(req, res)
    db.default().exec("DELETE FROM a41")
    res:json({ ok = true })
end)

app.get("/a41/:kind", function(req, res)
    local f = leg[req.params.kind]
    if not f then res:status(404); res:json({ unknown = true }); return end
    res:json(f(db.default(), db.connect("killer")))
end)
LUA
    cat > "$1/j/app.js" <<'JS'
import { app } from "hull:app";
import { db } from "hull:db";
app.manifest({ modules: ["hull/db@1", "hull/http-server@1"],
               databases: { named: { killer: "@DSN@" } } });

async function tryWait() {
    try {
        await hull.sleep(20);
        return [true, null];
    } catch (e) {
        return [false, e && e.code === "wait_refused" ? "wait_refused"
                                                      : String(e).slice(0, 120)];
    }
}

const count = (k) => Number(k.query("SELECT COUNT(*) AS n FROM a41")[0].n);
const attempt = (f) => { try { f(); return [true, null]; } catch (e) { return [false, String(e)]; } };

function kill(c, k) {
    const id = Number(c.query("SELECT CONNECTION_ID() AS id")[0].id);
    k.exec("KILL " + String(Math.trunc(id)));
}

function writesAfterKill(c, want) {
    let wrote = false, refused = false;
    for (let i = 1; i <= 3; i++) {
        const [ok, e] = attempt(() => c.exec("INSERT INTO a41 VALUES (" + (10 + i) + ")"));
        if (ok) wrote = true;
        if (!ok && e.includes(want)) refused = true;
    }
    return [wrote, refused];
}

const leg = {};
leg.kill_autocommit = async (c, k) => {
    c.exec("SET autocommit = 0");
    kill(c, k);
    const [wrote, refused] = writesAfterKill(c, "autocommit off");
    const [waited, code] = await tryWait();
    const [rollback_ok] = attempt(() => c.exec("ROLLBACK"));
    const [after_rollback_ok] = attempt(() => c.exec("INSERT INTO a41 VALUES (20)"));
    const ac_after = Number(c.query("SELECT @@session.autocommit AS a")[0].a);
    c.exec("ROLLBACK");
    c.exec("SET autocommit = 1");
    return { wrote, refused, waited, code, rollback_ok, after_rollback_ok,
             ac_after, rows: count(k) };
};
leg.kill_locktables = async (c, k) => {
    c.exec("LOCK TABLES a41 WRITE");
    kill(c, k);
    const [wrote, refused] = writesAfterKill(c, "LOCK TABLES");
    const [waited, code] = await tryWait();
    const [rollback_ok] = attempt(() => c.exec("ROLLBACK"));
    const [sel] = attempt(() => c.query("SELECT 1 AS x"));
    const [unlock_ok] = attempt(() => c.exec("UNLOCK TABLES"));
    const [after_unlock_ok] = attempt(() => c.query("SELECT COUNT(*) AS n FROM a41"));
    const [wait_after_unlock] = await tryWait();
    return { wrote, refused, waited, code, rollback_ok, still_refused: !sel,
             unlock_ok, after_unlock_ok, wait_after_unlock, rows: count(k) };
};
leg.kill_and_leave = async (c, k) => {
    c.exec("SET autocommit = 0");
    kill(c, k);
    const [wrote] = writesAfterKill(c, "autocommit off");
    return { wrote };
};
leg.probe = async (c, k) => {
    const ac = Number(c.query("SELECT @@session.autocommit AS a")[0].a);
    const [waited] = await tryWait();
    return { ac, waited, rows: count(k) };
};
leg.dash_comment = async (c, k) => {
    c.exec("LOCK TABLES a41 WRITE");
    c.query("SELECT 1 AS a --\fUNLOCK TABLES");
    const [waited, code] = await tryWait();
    c.exec("UNLOCK TABLES");
    const [wait_after_unlock] = await tryWait();
    return { waited, code, wait_after_unlock };
};
leg.call_txn = async (c, k) => {
    const [call_ok] = attempt(() => c.exec("CALL a41_txn()"));
    const in_txn = c.inTransaction();
    const [waited, code] = await tryWait();
    return { call_ok, in_txn, waited, code };
};
leg.call_ac = async (c, k) => {
    const [call_ok] = attempt(() => c.exec("CALL a41_ac()"));
    const [waited, code] = await tryWait();
    return { call_ok, waited, code };
};
leg.write60 = async (c, k) => { c.exec("INSERT INTO a41 VALUES (60)"); return { ok: true }; };
leg.write61 = async (c, k) => { c.exec("INSERT INTO a41 VALUES (61)"); return { ok: true }; };
leg.global_write = async (c, k) => {
    const ac = Number(c.query("SELECT @@session.autocommit AS a")[0].a);
    c.exec("INSERT INTO a41 VALUES (70)");
    return { ac };
};
leg.global_lock = async (c, k) => { c.exec("LOCK TABLES a41 WRITE"); return { ok: true }; };
leg.global_write2 = async (c, k) => {
    const ac = Number(c.query("SELECT @@session.autocommit AS a")[0].a);
    c.exec("INSERT INTO a41 VALUES (71)");
    return { ac };
};

app.get("/", (req, res) => res.json({ up: true }));

app.get("/setup", (req, res) => {
    const c = db.default();
    c.exec("DROP TABLE IF EXISTS a41");
    c.exec("CREATE TABLE a41 (v INT)");
    c.exec("DROP PROCEDURE IF EXISTS a41_txn");
    c.exec("DROP PROCEDURE IF EXISTS a41_ac");
    c.exec("CREATE PROCEDURE a41_txn() BEGIN START TRANSACTION; " +
           "INSERT INTO a41 VALUES (50); " +
           "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'a41 boom'; END");
    c.exec("CREATE PROCEDURE a41_ac() BEGIN SET autocommit = 0; " +
           "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'a41 boom'; END");
    res.json({ ok: true });
});

app.get("/reset", (req, res) => {
    db.default().exec("DELETE FROM a41");
    res.json({ ok: true });
});

app.get("/a41/:kind", async (req, res) => {
    const f = leg[req.params.kind];
    if (!f) { res.status(404); res.json({ unknown: true }); return; }
    res.json(await f(db.default(), db.connect("killer")));
});
JS
    for _f in "$1/l/app.lua" "$1/j/app.js"; do
        sed "s|@DSN@|$2|" "$_f" > "$_f.tmp" && mv "$_f.tmp" "$_f"
    done
}

db_session_audit41_run() {
    _port="$1"; shift
    for _kind in "$@"; do
        echo "A41 ${_kind} $(curl -s "http://127.0.0.1:${_port}/a41/${_kind}")"
    done
}

db_session_audit41_ok() {
    _out="$1"; _kind="$2"; shift 2
    _line=$(echo "$_out" | grep "^A41 $_kind " || true)
    for _want in "$@"; do
        case "$_line" in
            *"$_want"*) ;;
            *) echo "::error audit 41 ($_kind): want $_want in: $_line"
               return 1 ;;
        esac
    done
    return 0
}
