# shellcheck shell=sh
# hull_db_session_audit39.sh - the audit 39 app shared by e2e_mysql.sh and
# e2e_postgres.sh: a request that leaves session state on the shared default
# connection and raises must not poison the requests after it. Before the
# fix, after `XA START` (MySQL) every later write ran inside the zombie
# transaction and every wait was refused; after `SET autocommit = 0` every
# later write was rolled back at the end of its request; a LOCK TABLES
# refused every other table; a GET_LOCK / pg_advisory_lock taken by a failed
# request held every other session waiting on it - all until a restart.
#
# db_session_audit39_app DIR DIALECT   writes DIR/app.lua (DIALECT: mysql |
#                                      postgres)
# db_session_audit39_run PORT KIND...  for each KIND: GET /fail/KIND (must
#                                      answer 500), then GET /w/N (N = 1, 2,
#                                      ...), whose write must succeed and whose
#                                      wait must not be refused. Prints one
#                                      line per KIND:
#                                        A39 KIND fail=500 w={...}
#                                      The caller checks from another session
#                                      that row N of s39b is committed.

db_session_audit39_app() {
    cat > "$1/app.lua" <<'LUA'
app.manifest({ modules = { "hull/db@1", "hull/http-server@1" } })
local db = require("hull.db")
local DIALECT = "@DIALECT@"

local fail = {}
if DIALECT == "mysql" then
    -- An XA transaction: ROLLBACK is refused (XAER_RMFAIL), IN_TRANS stays.
    fail.xa = function(c)
        c.exec("XA START 'a39'")
        c.exec("INSERT INTO s39 VALUES (101)")
    end
    -- Autocommit off: each later statement opens a transaction.
    fail.autocommit = function(c)
        c.exec("SET autocommit = 0")
        c.exec("INSERT INTO s39 VALUES (102)")
    end
    -- LOCK TABLES with no transaction open: every other table refused.
    fail.locktables = function(c)
        c.exec("LOCK TABLES s39 WRITE")
        c.exec("INSERT INTO s39 VALUES (103)")
    end
    -- A named lock taken by a request that then failed mid-transaction.
    fail.getlock = function(c)
        c.query("SELECT GET_LOCK('a39lk', 0) AS g")
        c.exec("START TRANSACTION")
        c.exec("INSERT INTO s39 VALUES (104)")
    end
else
    -- A session-level advisory lock outlives the ROLLBACK.
    fail.advisory = function(c)
        c.query("SELECT pg_advisory_lock(3939) AS g")
        c.exec("BEGIN")
        c.exec("INSERT INTO s39 VALUES (105)")
    end
end

app.get("/", function(req, res) res:json({ up = true }) end)   -- readiness

app.get("/setup", function(req, res)
    local c = db.default()
    c.exec("DROP TABLE IF EXISTS s39")
    c.exec("DROP TABLE IF EXISTS s39b")
    c.exec("CREATE TABLE s39 (v INT)")
    c.exec("CREATE TABLE s39b (v INT)")
    res:json({ ok = true })
end)

app.get("/fail/:kind", function(req, res)
    local f = fail[req.params.kind]
    if not f then res:status(404); res:json({ unknown = true }); return end
    f(db.default())
    error("the handler failed and left its session state behind")
end)

-- An unrelated later request: its write must commit, and its wait must not
-- be refused.
app.get("/w/:v", function(req, res)
    local c = db.default()
    local ok_w, err_w = pcall(c.exec, "INSERT INTO s39b VALUES (?)",
                              { tonumber(req.params.v) })
    local ok_s = pcall(hull.sleep, 1)
    res:json({ write = ok_w, in_txn = c.in_transaction(), sleep_ok = ok_s,
               err = (not ok_w) and tostring(err_w):sub(1, 80) or nil })
end)

if DIALECT == "mysql" then
    -- The connection character set survives a session reset.
    app.get("/charset", function(req, res)
        local r = db.default().query(
            "SELECT @@character_set_client AS c, @@collation_connection AS k")[1]
        res:json({ cs = r.c .. "/" .. r.k })
    end)
end
LUA
    # The dialect is substituted in after the heredoc (kept quoted above so
    # the Lua text needs no shell escaping).
    sed "s/@DIALECT@/$2/" "$1/app.lua" > "$1/app.lua.tmp" && mv "$1/app.lua.tmp" "$1/app.lua"
}

db_session_audit39_run() {
    _port="$1"; shift
    _n=0
    for _kind in "$@"; do
        _n=$((_n + 1))
        _code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${_port}/fail/${_kind}")
        _w=$(curl -s "http://127.0.0.1:${_port}/w/${_n}")
        echo "A39 ${_kind} fail=${_code} w=${_w}"
    done
}

# db_session_audit39_ok OUT KIND: the KIND line of OUT shows the failed
# request answered 500 and the next one wrote, waited and ended outside a
# transaction. Prints an ::error line and returns 1 otherwise.
db_session_audit39_ok() {
    _line=$(echo "$1" | grep "^A39 $2 " || true)
    for _want in 'fail=500' '"write":true' '"in_txn":false' '"sleep_ok":true'; do
        case "$_line" in
            *"$_want"*) ;;
            *) echo "::error audit 39 ($2): the request after the failed one: $_line"
               return 1 ;;
        esac
    done
    return 0
}
