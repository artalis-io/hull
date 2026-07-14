#!/bin/sh
# e2e_dynamic_connections.sh: E2E for db.open() dynamic connections (roadmap §2.2)
#
# Proves the caller-owned connection path end-to-end through the built binary in
# both runtimes: a runtime-computed DSN is validated against
# manifest.databases.dynamic (scheme + fs allowlist), opens a caller-owned
# connection, queries it, and is released by close() (double-close idempotent,
# use-after-close fails closed). A denied scheme and an out-of-sandbox file path
# are rejected before any connection. SQLite only, so no Docker; the Postgres
# host-allowlist (CIDR) allow/deny variant lives in tests/e2e_postgres.sh and
# the C unit test tests/hull/cap/test_db_dynamic.c.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
# run_case cd's into the app dir, so the hull path must be absolute.
case "$HULL" in /*) ;; *) HULL="$PWD/$HULL" ;; esac
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# The app opens an allowed sqlite :memory: connection, exercises it, closes it
# (twice), then proves a closed handle and a denied scheme/path all fail. It
# emits one KEY=VALUE line per assertion so the shell can grep the outcomes.
# Unique variable names: POSIX sh has no locals, so a name reused here would
# clobber run_case's $label between calls.
assert_line() {
    a_out="$1"; a_key="$2"; a_want="$3"; a_lbl="$4"
    a_got=$(printf '%s\n' "$a_out" | sed -n "s/.*${a_key}=\\([^ ]*\\).*/\\1/p" | head -1)
    [ "$a_got" = "$a_want" ] && pass "$a_lbl" || fail "$a_lbl" "$a_got"
}

# Run from inside TMPDIR so the app dir is the sandbox base and a relative file
# DSN ("shard.db") resolves consistently for both the sync connection and the
# async worker thread (both cwd-relative in one process).
run_case() {
    ext="$1"; label="$2"
    rm -f "$TMPDIR/shard.db"
    out=$(cd "$TMPDIR" && "$HULL" --no-sandbox "app.$ext" 2>&1 || true)
    assert_line "$out" "open_backend"   "sqlite" "$label: allowed scheme opens"
    assert_line "$out" "query_x"        "42"     "$label: query on owned conn"
    assert_line "$out" "double_close"   "ok"     "$label: double close is idempotent"
    assert_line "$out" "use_after"      "denied" "$label: use-after-close fails closed"
    assert_line "$out" "deny_scheme"    "denied" "$label: scheme not in allowlist rejected"
    assert_line "$out" "deny_path"      "denied" "$label: out-of-sandbox file path rejected"
    # async targets the SAME dynamic (file) DB the sync side wrote to, and fails
    # closed after close().
    assert_line "$out" "async_x"           "7"      "$label: async targets the dynamic DB"
    assert_line "$out" "async_after_close" "denied" "$label: async-after-close fails closed"
}

# ── Lua ────────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.lua" << 'LUA'
app.manifest({
    modules = { "hull/db@1" },
    fs = { read = { "shard.db" }, write = { "shard.db" } },
    databases = { dynamic = { schemes = { "sqlite" } } },
})
local db = require("hull.db")
app.main(function()
    local c = db.open(":memory:")
    print("open_backend=" .. c.backend_name)
    c.exec("CREATE TABLE t(x INTEGER)")
    c.exec("INSERT INTO t(x) VALUES (?)", { 42 })
    print("query_x=" .. tostring(c.query("SELECT x FROM t")[1].x))
    c.close()
    local ok_dc = pcall(function() c.close() end)
    print("double_close=" .. (ok_dc and "ok" or "err"))
    local ok_use = pcall(function() c.query("SELECT 1") end)
    print("use_after=" .. (ok_use and "ok" or "denied"))
    local ok_sc = pcall(function() return db.open("postgres://u@localhost/db") end)
    print("deny_scheme=" .. (ok_sc and "ok" or "denied"))
    local ok_pp = pcall(function() return db.open("/etc/hosts") end)
    print("deny_path=" .. (ok_pp and "ok" or "denied"))
    -- async on a file-backed dynamic connection sees the sync-written row.
    local f = db.open("shard.db")
    f.exec("CREATE TABLE t(x INTEGER)")
    f.exec("INSERT INTO t(x) VALUES (?)", { 7 })
    local r = f.async.query("SELECT x FROM t")
    print("async_x=" .. tostring(r[1] and r[1].x))
    f.close()
    local ok_a = pcall(function() f.async.query("SELECT 1") end)
    print("async_after_close=" .. (ok_a and "ok" or "denied"))
    return 0
end)
LUA
run_case lua "Lua"

# ── JS ─────────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.js" << 'JS'
import { app } from "hull:app";
import { db as dbMod } from "hull:db";
app.manifest({
    modules: ["hull/db@1"],
    fs: { read: ["shard.db"], write: ["shard.db"] },
    databases: { dynamic: { schemes: ["sqlite"] } },
});
app.main(async () => {
    const c = dbMod.open(":memory:");
    console.log("open_backend=" + c.backendName);
    c.exec("CREATE TABLE t(x INTEGER)");
    c.exec("INSERT INTO t(x) VALUES (?)", [42]);
    console.log("query_x=" + c.query("SELECT x FROM t")[0].x);
    c.close();
    let dc = "ok"; try { c.close(); } catch (e) { dc = "err"; }
    console.log("double_close=" + dc);
    let ua = "ok"; try { c.query("SELECT 1"); } catch (e) { ua = "denied"; }
    console.log("use_after=" + ua);
    let sc = "ok"; try { dbMod.open("postgres://u@localhost/db"); } catch (e) { sc = "denied"; }
    console.log("deny_scheme=" + sc);
    let pp = "ok"; try { dbMod.open("/etc/hosts"); } catch (e) { pp = "denied"; }
    console.log("deny_path=" + pp);
    // async on a file-backed dynamic connection sees the sync-written row.
    const f = dbMod.open("shard.db");
    f.exec("CREATE TABLE t(x INTEGER)");
    f.exec("INSERT INTO t(x) VALUES (?)", [7]);
    const r = await f.async.query("SELECT x FROM t");
    console.log("async_x=" + (r[0] && r[0].x));
    f.close();
    let aa = "ok"; try { await f.async.query("SELECT 1"); } catch (e) { aa = "denied"; }
    console.log("async_after_close=" + aa);
    return 0;
});
JS
run_case js "JS"

echo ""
echo "dynamic connections: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
