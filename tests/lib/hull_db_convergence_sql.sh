# shellcheck shell=sh
# hull_db_convergence_sql.sh - the audit convergence §7.1 backstop legs shared
# by e2e_mysql.sh and e2e_postgres.sh.
#
#   drain   A request waits on an in-process promise (long-poll style); a
#           second request sets per-request session state (a user variable /
#           set_config GUC, the time zone) and settles it. The first
#           request's continuation runs in the second one's drain: it must
#           not read the second request's session. The reset ran only when a
#           transaction was open there, so it did (JS; Lua has no shared job
#           queue - each resume is guarded).
#   role    (MySQL) A role set through PREPARE from a user variable, or by a
#           CALLed procedure - neither visible to the session-statement
#           refusal - and a USE are gone in the next entry:
#           COM_RESET_CONNECTION leaves the active role and the current
#           database as they were, so the reset puts the login's back.
#
# db_convergence_sql_apps DIR      writes DIR/drain/app.js and DIR/role/app.js
# db_convergence_sql_drain HULL DSN DIR PORT
#                                  runs the drain leg; 0 = pass
# db_convergence_sql_role HULL DSN DIR PORT
#                                  runs the role leg (the role hull_cs_rr, the
#                                  procedure hull_cs_set_rr and the database
#                                  hull_cs_db2 must exist and be usable by the
#                                  DSN's user); 0 = pass

db_convergence_sql_apps() {
    mkdir -p "$1/drain" "$1/role"
    cat > "$1/drain/app.js" <<'JS'
import { app } from "hull:app";
import { db } from "hull:db";
app.manifest({ modules: ["hull/db@1", "hull/http-server@1"] });
let release = null;
let gate = null;
function newGate() { gate = new Promise((r) => { release = r; }); }
newGate();
const READ_MY = "SELECT @cs_actor AS actor, @@session.time_zone AS tz";
const READ_PG = "SELECT current_setting('cs.actor', true) AS actor, " +
                "current_setting('TimeZone') AS tz";
function pg(c) { return c.backendName === "postgres"; }
function read(c) { return c.query(pg(c) ? READ_PG : READ_MY)[0]; }
function setActor(c, who) {
    if (pg(c)) {
        c.query("SELECT set_config('cs.actor', ?, false)", [who]);
        c.exec("SET TIME ZONE 'Asia/Tokyo'");
    } else {
        c.exec("SET @cs_actor = ?", [who]);
        c.exec("SET time_zone = '+09:00'");
    }
}
app.get("/", (req, res) => res.json({ up: true }));
// The victim waits on an in-process event, having used the connection.
// Hull ends a request that awaits a promise it does not drive, but the code
// after the await still runs - in the drain of the request that settles the
// promise - so what it read is kept for /seen.
let seen = null;
app.get("/wait", async (req, res) => {
    const c = db.default();
    read(c);
    await gate;
    seen = read(c);
});
app.get("/seen", (req, res) => res.json({ seen }));
// Another request sets its own session state, then fires the event.
app.get("/fire", (req, res) => {
    const c = db.default();
    setActor(c, "other-request");
    const own = read(c);
    const rel = release; newGate(); rel();
    res.json({ own });
});
JS

    cat > "$1/role/app.js" <<'JS'
import { app } from "hull:app";
import { db } from "hull:db";
app.manifest({ modules: ["hull/db@1", "hull/http-server@1"] });
const ROLE = "SELECT COALESCE(CURRENT_ROLE(), 'NONE') AS r, DATABASE() AS d";
function role(c) { const r = c.query(ROLE)[0]; return String(r.r) + "/" + String(r.d); }
app.get("/", (req, res) => res.json({ up: true }));
app.get("/prep", (req, res) => {
    const c = db.default();
    c.exec("SET @cs_q = CONCAT('SET R', 'OLE hull_cs_rr')");
    c.exec("PREPARE cs_s FROM @cs_q");
    c.exec("EXECUTE cs_s");
    res.json({ role: role(c) });
});
app.get("/call", (req, res) => {
    const c = db.default();
    c.exec("CALL hull_cs_set_rr()");
    res.json({ role: role(c) });
});
app.get("/use", (req, res) => {
    const c = db.default();
    c.exec("USE hull_cs_db2");
    res.json({ role: role(c) });
});
app.get("/check", (req, res) => res.json({ role: role(db.default()) }));
JS
}

_cs_wait_up() {
    _i=0
    while [ "$_i" -lt 60 ]; do
        curl -fsS "http://127.0.0.1:$1/" >/dev/null 2>&1 && return 0
        _i=$((_i + 1)); sleep 0.5
    done
    return 1
}

db_convergence_sql_drain() {
    _hull=$1; _dsn=$2; _dir=$3; _port=$4
    "$_hull" -d "$_dsn" -p "$_port" "$_dir/drain/app.js" >"$_dir/drain.log" 2>&1 &
    _svr=$!
    if ! _cs_wait_up "$_port"; then
        kill "$_svr" 2>/dev/null; cat "$_dir/drain.log"; return 1
    fi
    curl -s --max-time 15 "http://127.0.0.1:${_port}/wait" >"$_dir/wait.out" &
    _w=$!
    sleep 1
    _fire=$(curl -s "http://127.0.0.1:${_port}/fire")
    wait "$_w" 2>/dev/null || true
    _wait=$(curl -s "http://127.0.0.1:${_port}/seen")
    kill "$_svr" 2>/dev/null || true; wait "$_svr" 2>/dev/null || true
    echo "fire: $_fire"
    echo "seen: $_wait"
    _rc=0
    case "$_fire" in
        *other-request*) ;;
        *) echo "::error convergence drain: /fire did not set its state: $_fire"; _rc=1 ;;
    esac
    case "$_wait" in
        *'"seen":{'*) ;;
        *) echo "::error convergence drain: the waiting request's continuation did not run: $_wait"; _rc=1 ;;
    esac
    case "$_wait" in
        *other-request*|*Asia/Tokyo*|*'+09:00'*)
            echo "::error convergence drain: the waiting request read the other request's session: $_wait"
            _rc=1 ;;
    esac
    [ "$_rc" = 0 ] || cat "$_dir/drain.log"
    return $_rc
}

db_convergence_sql_role() {
    _hull=$1; _dsn=$2; _dir=$3; _port=$4
    "$_hull" -d "$_dsn" -p "$_port" "$_dir/role/app.js" >"$_dir/role.log" 2>&1 &
    _svr=$!
    if ! _cs_wait_up "$_port"; then
        kill "$_svr" 2>/dev/null; cat "$_dir/role.log"; return 1
    fi
    _base=$(curl -s "http://127.0.0.1:${_port}/check")
    _prep=$(curl -s "http://127.0.0.1:${_port}/prep")
    _after_prep=$(curl -s "http://127.0.0.1:${_port}/check")
    _call=$(curl -s "http://127.0.0.1:${_port}/call")
    _after_call=$(curl -s "http://127.0.0.1:${_port}/check")
    _use=$(curl -s "http://127.0.0.1:${_port}/use")
    _after_use=$(curl -s "http://127.0.0.1:${_port}/check")
    kill "$_svr" 2>/dev/null || true; wait "$_svr" 2>/dev/null || true
    echo "base: $_base prep: $_prep after: $_after_prep call: $_call after: $_after_call use: $_use after: $_after_use"
    _rc=0
    case "$_prep" in *hull_cs_rr*) ;; *) echo "::error convergence role: PREPARE did not set the role: $_prep"; _rc=1 ;; esac
    case "$_call" in *hull_cs_rr*) ;; *) echo "::error convergence role: CALL did not set the role: $_call"; _rc=1 ;; esac
    case "$_use" in *hull_cs_db2*) ;; *) echo "::error convergence role: USE did not switch: $_use"; _rc=1 ;; esac
    case "$_base" in *'"role":'*) ;; *) echo "::error convergence role: /check did not answer: $_base"; _rc=1 ;; esac
    for _c in "$_after_prep" "$_after_call" "$_after_use"; do
        [ "$_c" = "$_base" ] || { echo "::error convergence role: role / database carried into the next entry: $_c (login: $_base)"; _rc=1; }
    done
    [ "$_rc" = 0 ] || cat "$_dir/role.log"
    return $_rc
}
