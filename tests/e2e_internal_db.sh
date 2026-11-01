#!/bin/sh
# e2e_internal_db.sh: E2E for the Hull-owned internal database (audit
# convergence §7.2, docs/design/internal_db_separation.md).
#
# With no `databases.internal` declared, the stdlib's own _hull_* tables
# (sessions, RBAC, TOTP, audit log, idempotency, auth-flows, attachments)
# live in <app>/.hull/internal.db - or beside a -d database outside the app
# directory - never in the app's database file. This proves, in both
# runtimes:
#   - a fresh app creates them in the internal database only;
#   - an existing app (tables in data.db, written under the old layout with
#     --keep-internal-in-app-db) has them moved on first start, rows intact;
#   - --keep-internal-in-app-db moves them back, and a later start moves
#     them in again;
#   - a start interrupted between tables (the crash state: some tables
#     copied but not yet dropped) is completed with no row lost or doubled;
#   - _hull_* tables planted in data.db after the move are ignored, not
#     merged into the live internal data;
#   - a -d database outside the app directory gets its internal database
#     beside it; a declared databases.internal wins; a databases.named DSN
#     may not name .hull;
#   - on macOS, the kernel sandbox (Seatbelt) still lets Hull use the file.
# Python's sqlite3 inspects the databases.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
case "$HULL" in /*) ;; *) HULL="$(pwd)/$HULL" ;; esac
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; echo "        got: $2" | head -c 3000; echo; }

PY=""
for p in python3 python; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c "import sqlite3" >/dev/null 2>&1; then
        PY="$p"; break
    fi
done
if [ -z "$PY" ]; then
    echo "SKIP: e2e_internal_db needs python with sqlite3 to inspect the databases"
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
WTMP=$(cd "$TMPDIR" && (pwd -W 2>/dev/null || pwd))

# sql DB QUERY -> the first column of the first row ("" when none / no file)
sql() {
    "$PY" - "$1" "$2" <<'PYEOF'
import sqlite3, sys, os
if not os.path.exists(sys.argv[1]):
    print(""); sys.exit(0)
c = sqlite3.connect(sys.argv[1])
try:
    r = c.execute(sys.argv[2]).fetchone()
    print("" if r is None else r[0])
except sqlite3.Error as e:
    print("ERR:" + str(e))
PYEOF
}
has_table() { sql "$1" "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='$2'"; }

write_app() {
    dir="$1"; ext="$2"
    mkdir -p "$dir"
    if [ "$ext" = lua ]; then
        cat > "$dir/app.lua" <<'EOF'
app.manifest({ modules = { "hull/web/middleware/session@1",
                           "hull/web/middleware/rbac@1", "hull/db@1" } })
local session = require("hull.web.middleware.session")
local rbac = require("hull.web.middleware.rbac")
app.main(function(ctx)
    session.init({})
    rbac.init()
    if ctx.args[1] == "seed" then
        rbac.define_role("admin", {})
        rbac.assign("alice", "admin")
        ctx.stdout:write("sid=" .. session.create({ user_id = "alice" }) .. "\n")
    else
        ctx.stdout:write("roles=" .. table.concat(rbac.roles("alice"), ",") .. "\n")
        if ctx.args[1] then
            ctx.stdout:write("session=" .. tostring(session.load(ctx.args[1]) ~= nil) .. "\n")
        end
        -- What app SQL on the app database can see of Hull's tables.
        local rows = require("hull.db").default().query(
            "SELECT count(*) AS n FROM sqlite_master WHERE type = 'table' " ..
            "AND substr(name, 1, 6) = '_' || 'hull' || '_' " ..
            "AND substr(name, 7) <> 'migrations'")
        ctx.stdout:write("app_db_hull_tables=" .. tostring(rows[1].n) .. "\n")
    end
    return 0
end)
EOF
    else
        cat > "$dir/app.js" <<'EOF'
import { app } from "hull:app";
import { session } from "hull:web:middleware:session";
import { rbac } from "hull:web:middleware:rbac";
import { db } from "hull:db";
app.manifest({ modules: ["hull/web/middleware/session@1",
                         "hull/web/middleware/rbac@1", "hull/db@1"] });
app.main((ctx) => {
    session.init({});
    rbac.init();
    if (ctx.args[0] === "seed") {
        rbac.defineRole("admin", []);
        rbac.assign("alice", "admin");
        ctx.stdout.write("sid=" + session.create({ user_id: "alice" }) + "\n");
    } else {
        ctx.stdout.write("roles=" + rbac.roles("alice").join(",") + "\n");
        if (ctx.args[0])
            ctx.stdout.write("session=" + (session.load(ctx.args[0]) != null) + "\n");
        const rows = db.default().query(
            "SELECT count(*) AS n FROM sqlite_master WHERE type = 'table' " +
            "AND substr(name, 1, 6) = '_' || 'hull' || '_' " +
            "AND substr(name, 7) <> 'migrations'");
        ctx.stdout.write("app_db_hull_tables=" + rows[0].n + "\n");
    }
    return 0;
});
EOF
    fi
}

# run DIR EXT [hull flags...] -- [app args...]
run() {
    dir="$1"; ext="$2"; shift 2
    (cd "$dir" && "$HULL" "$@" 2>&1) || true
}
# The app's arguments go after "--"; hull's flags (and the entry) before.
hrun() {
    dir="$1"; ext="$2"; shift 2
    flags=""
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do flags="$flags $1"; shift; done
    [ "$1" = "--" ] && shift
    # shellcheck disable=SC2086
    (cd "$dir" && "$HULL" --no-sandbox $flags -d data.db "app.$ext" -- "$@" 2>&1) || true
}
field() { echo "$1" | sed -n "s/^$2=//p" | head -1; }

for ext in lua js; do
    echo "=== $ext: fresh app ==="
    d="$TMPDIR/fresh_$ext"
    write_app "$d" "$ext"
    out=$(hrun "$d" "$ext" -- seed)
    sid=$(field "$out" sid)
    out2=$(hrun "$d" "$ext" -- "$sid")
    [ "$(has_table "$d/.hull/internal.db" _hull_sessions)" = 1 ] \
        && [ "$(has_table "$d/.hull/internal.db" _hull_user_roles)" = 1 ] \
        && pass "$ext: a fresh app keeps its _hull_* tables in .hull/internal.db" \
        || fail "$ext: a fresh app keeps its _hull_* tables in .hull/internal.db" "$out"
    [ "$(has_table "$d/data.db" _hull_sessions)" = 0 ] \
        && [ "$(has_table "$d/data.db" _hull_roles)" = 0 ] \
        && pass "$ext: the app database holds no internal table" \
        || fail "$ext: the app database holds no internal table" "$out"
    [ "$(has_table "$d/data.db" _hull_migrations)" = 1 ] \
        && pass "$ext: _hull_migrations stays with the app database" \
        || fail "$ext: _hull_migrations stays with the app database" "$out"
    case "$out2" in
        *"roles=admin"*"session=true"*"app_db_hull_tables=0"*)
            pass "$ext: sessions and roles persist across starts; app SQL sees no internal table" ;;
        *) fail "$ext: sessions and roles persist across starts; app SQL sees no internal table" "$out2" ;;
    esac
    mode=$("$PY" -c "import os,stat,sys; print(oct(stat.S_IMODE(os.stat(sys.argv[1]).st_mode)))" "$d/.hull")
    [ "$mode" = 0o700 ] && pass "$ext: .hull is made 0700" || fail "$ext: .hull is made 0700" "$mode"
    [ "$(sql "$d/.hull/internal.db" "SELECT count(*) FROM _hull_internal_meta WHERE key='app_db_moved'")" = 1 ] \
        && pass "$ext: the internal database records the move as done" \
        || fail "$ext: the internal database records the move as done" "$out"

    echo "=== $ext: existing app (old layout) ==="
    d="$TMPDIR/old_$ext"
    write_app "$d" "$ext"
    out=$(hrun "$d" "$ext" --keep-internal-in-app-db -- seed)
    sid=$(field "$out" sid)
    [ "$(has_table "$d/data.db" _hull_sessions)" = 1 ] && [ ! -f "$d/.hull/internal.db" ] \
        && pass "$ext: --keep-internal-in-app-db writes the old layout" \
        || fail "$ext: --keep-internal-in-app-db writes the old layout" "$out"
    out=$(hrun "$d" "$ext" -- "$sid")
    case "$out" in
        *"moved 5 stdlib table(s)"*) pass "$ext: the first start moves the tables, with a notice" ;;
        *) fail "$ext: the first start moves the tables, with a notice" "$out" ;;
    esac
    case "$out" in
        *"roles=admin"*"session=true"*"app_db_hull_tables=0"*)
            pass "$ext: moved sessions and roles still work" ;;
        *) fail "$ext: moved sessions and roles still work" "$out" ;;
    esac
    [ "$(has_table "$d/data.db" _hull_sessions)" = 0 ] \
        && [ "$(has_table "$d/data.db" _hull_user_roles)" = 0 ] \
        && [ "$(sql "$d/.hull/internal.db" "SELECT count(*) FROM _hull_sessions")" = 1 ] \
        && [ "$(sql "$d/.hull/internal.db" "SELECT count(*) FROM _hull_user_roles")" = 1 ] \
        && pass "$ext: the rows are in the internal database and gone from data.db" \
        || fail "$ext: the rows are in the internal database and gone from data.db" "$out"
    out=$(hrun "$d" "$ext" -- "$sid")
    case "$out" in
        *"moved "*) fail "$ext: a second start moves nothing" "$out" ;;
        *"roles=admin"*"session=true"*) pass "$ext: a second start moves nothing" ;;
        *) fail "$ext: a second start moves nothing" "$out" ;;
    esac

    echo "=== $ext: --keep-internal-in-app-db moves them back ==="
    out=$(hrun "$d" "$ext" --keep-internal-in-app-db -- "$sid")
    case "$out" in
        *"moved 5 stdlib table(s) back"*"roles=admin"*"session=true"*)
            pass "$ext: the keep flag moves the tables back and they work" ;;
        *) fail "$ext: the keep flag moves the tables back and they work" "$out" ;;
    esac
    [ "$(sql "$d/data.db" "SELECT count(*) FROM _hull_sessions")" = 1 ] \
        && [ "$(has_table "$d/.hull/internal.db" _hull_sessions)" = 0 ] \
        && [ "$(sql "$d/.hull/internal.db" "SELECT count(*) FROM _hull_internal_meta")" = 0 ] \
        && pass "$ext: back in data.db, out of the internal database, move record cleared" \
        || fail "$ext: back in data.db, out of the internal database, move record cleared" "$out"

    echo "=== $ext: a start interrupted between tables ==="
    # The state a crash after some tables were copied (committed) but before
    # any was dropped leaves: _hull_sessions and _hull_roles on both sides,
    # the rest only in data.db, no move record.
    "$PY" - "$d/data.db" "$d/.hull/internal.db" <<'PYEOF'
import sqlite3, sys
src = sqlite3.connect(sys.argv[1]); dst = sqlite3.connect(sys.argv[2])
for t in ("_hull_sessions", "_hull_roles"):
    for (ddl,) in src.execute("SELECT sql FROM sqlite_master WHERE tbl_name=? "
                              "AND sql IS NOT NULL ORDER BY type='index'", (t,)):
        dst.execute(ddl)
    cols = [r[1] for r in src.execute("PRAGMA table_info(%s)" % t)]
    for row in src.execute("SELECT * FROM %s" % t):
        dst.execute("INSERT INTO %s (%s) VALUES (%s)" % (t, ",".join(cols), ",".join("?" * len(cols))), row)
dst.commit()
PYEOF
    out=$(hrun "$d" "$ext" -- "$sid")
    case "$out" in
        *"roles=admin"*"session=true"*"app_db_hull_tables=0"*)
            pass "$ext: the interrupted move is completed on the next start" ;;
        *) fail "$ext: the interrupted move is completed on the next start" "$out" ;;
    esac
    [ "$(sql "$d/.hull/internal.db" "SELECT count(*) FROM _hull_sessions")" = 1 ] \
        && [ "$(sql "$d/.hull/internal.db" "SELECT count(*) FROM _hull_roles")" = 1 ] \
        && [ "$(sql "$d/.hull/internal.db" "SELECT count(*) FROM _hull_user_roles")" = 1 ] \
        && pass "$ext: no row lost or doubled" \
        || fail "$ext: no row lost or doubled" "$(sql "$d/.hull/internal.db" "SELECT count(*) FROM _hull_sessions")"

    echo "=== $ext: _hull_* tables planted in data.db after the move ==="
    "$PY" - "$d/data.db" <<'PYEOF'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
c.execute("CREATE TABLE _hull_user_roles (user_id VARCHAR(255), role VARCHAR(255))")
c.execute("INSERT INTO _hull_user_roles VALUES ('mallory', 'admin')")
c.commit()
PYEOF
    out=$(hrun "$d" "$ext" -- "$sid")
    case "$out" in
        *"Hull no longer reads"*) pass "$ext: planted tables are reported and ignored" ;;
        *) fail "$ext: planted tables are reported and ignored" "$out" ;;
    esac
    [ "$(sql "$d/.hull/internal.db" "SELECT count(*) FROM _hull_user_roles WHERE user_id='mallory'")" = 0 ] \
        && pass "$ext: planted rows never reach the internal database" \
        || fail "$ext: planted rows never reach the internal database" "$out"

    echo "=== $ext: -d outside the app directory ==="
    d="$TMPDIR/out_$ext"
    write_app "$d" "$ext"
    mkdir -p "$TMPDIR/vol_$ext"
    out=$(cd "$d" && "$HULL" --no-sandbox -d "$WTMP/vol_$ext/app.db" "app.$ext" -- seed 2>&1 || true)
    [ "$(has_table "$TMPDIR/vol_$ext/app.db.hull-internal" _hull_sessions)" = 1 ] \
        && [ "$(has_table "$TMPDIR/vol_$ext/app.db" _hull_sessions)" = 0 ] \
        && [ ! -e "$d/.hull/internal.db" ] \
        && pass "$ext: the internal database sits beside a -d database outside the app" \
        || fail "$ext: the internal database sits beside a -d database outside the app" "$out"

    echo "=== $ext: a declared databases.internal wins ==="
    d="$TMPDIR/decl_$ext"
    write_app "$d" "$ext"
    if [ "$ext" = lua ]; then
        sed -i.bak 's|"hull/db@1" } })|"hull/db@1" }, databases = { internal = "mine.db" } })|' "$d/app.lua"
    else
        sed -i.bak 's|"hull/db@1"] });|"hull/db@1"], databases: { internal: "mine.db" } });|' "$d/app.js"
    fi
    out=$(hrun "$d" "$ext" -- seed)
    [ "$(has_table "$d/mine.db" _hull_sessions)" = 1 ] && [ ! -e "$d/.hull/internal.db" ] \
        && pass "$ext: the declared internal database is used, .hull/internal.db is not made" \
        || fail "$ext: the declared internal database is used, .hull/internal.db is not made" "$out"
done

echo "=== databases.named may not name .hull ==="
d="$TMPDIR/named"
mkdir -p "$d"
cat > "$d/app.lua" <<'EOF'
app.manifest({ modules = { "hull/db@1" }, databases = { named = { peek = ".HULL/internal.db" } } })
app.main(function(ctx)
    local ok, err = pcall(function() return require("hull.db").connect("peek") end)
    ctx.stdout:write("peek=" .. tostring(ok) .. " " .. tostring(err) .. "\n")
    return 0
end)
EOF
out=$(hrun "$d" lua)
case "$out" in
    *"peek=false"*".hull directory"*) pass "a databases.named DSN naming .hull is refused" ;;
    *) fail "a databases.named DSN naming .hull is refused" "$out" ;;
esac

echo "=== an app with no module that needs it gets no internal database ==="
d="$TMPDIR/plain"
mkdir -p "$d"
cat > "$d/app.lua" <<'EOF'
app.manifest({ modules = { "hull/db@1" } })
app.main(function(ctx)
    require("hull.db").default().exec("CREATE TABLE IF NOT EXISTS t (x INTEGER)")
    ctx.stdout:write("ok\n")
    return 0
end)
EOF
out=$(hrun "$d" lua)
case "$out" in
    *ok*) [ ! -e "$d/.hull" ] \
        && pass "no .hull/internal.db is made for an app without session / rbac / ..." \
        || fail "no .hull/internal.db is made for an app without session / rbac / ..." "$out" ;;
    *) fail "no .hull/internal.db is made for an app without session / rbac / ..." "$out" ;;
esac

if [ "$(uname -s)" = Darwin ]; then
    echo "=== macOS: the kernel sandbox lets Hull use the internal database ==="
    d="$TMPDIR/sb"
    write_app "$d" lua
    out=$(cd "$d" && "$HULL" -d data.db app.lua -- seed 2>&1 || true)
    sid=$(field "$out" sid)
    out=$(cd "$d" && "$HULL" -d data.db app.lua -- "$sid" 2>&1 || true)
    case "$out" in
        *"[sandbox] applied"*"roles=admin"*"session=true"*)
            pass "Seatbelt: sessions and roles are read from .hull/internal.db after the sandbox applies" ;;
        *) fail "Seatbelt: sessions and roles are read from .hull/internal.db after the sandbox applies" "$out" ;;
    esac
fi

echo ""
echo "e2e_internal_db: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
