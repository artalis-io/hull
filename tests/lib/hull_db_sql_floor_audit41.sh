# hull_db_sql_floor_audit41.sh - shared by e2e_postgres.sh and e2e_mysql.sh.
#
# Audit 41: a statement sent to a network database paid only a charge per
# reply frame (~1 unit for a ~100-250 us round trip), so a budget-sized loop
# of `SELECT 1` held the event loop for minutes. Each statement now pays
# HL_WORK_NET_SQL_UNITS (8192) before it is sent.
#
#   db_sql_floor_audit41 HULL DSN WORKDIR LABEL
#
# Runs, in both runtimes: 1000 pcall'd `SELECT 1` under a 2M budget (must
# trip: ~8.2M units), and 100 of them (must fit: ~0.8M). Returns non-zero
# with the output on failure.

db_sql_floor_audit41() {
    _h=$1; _dsn=$2; _d=$3; _label=$4
    mkdir -p "$_d/l" "$_d/j"
    cat > "$_d/l/app.lua" <<'LUA'
app.manifest({ modules = { "hull/db@1" } })
local db = require("hull.db")
app.main(function(ctx)
  local conn = db.default()
  local n = tonumber(ctx.args[1])
  for i = 1, n do pcall(conn.query, "SELECT 1 AS one") end
  ctx.stdout:write("DONE " .. n .. "\n")
  return 0
end)
LUA
    cat > "$_d/j/app.js" <<'JS'
import { app } from "hull:app";
import { db as dbm } from "hull:db";
app.manifest({ modules: ["hull/db@1"] });
app.main((ctx) => {
  const conn = dbm.default();
  const n = Number(ctx.args[0]);
  for (let i = 0; i < n; i++) { try { conn.query("SELECT 1 AS one"); } catch (e) {} }
  ctx.stdout.write("DONE " + n + "\n");
  return 0;
});
JS
    for _app in "$_d/l/app.lua" "$_d/j/app.js"; do
        _out=$(HULL_MAX_INSTRUCTIONS=2000000 "$_h" "$_app" -d "$_dsn" -- 1000 2>&1) || true
        case "$_out" in
            *"DONE 1000"*|*"instruction limit"*) ;;
            *) echo "::error audit 41 ($_label $_app): unexpected: $_out"; return 1 ;;
        esac
        case "$_out" in
            *"DONE 1000"*)
                echo "::error audit 41 ($_label $_app): 1000 SELECT 1 fit a 2M budget (no per-statement floor)"
                return 1 ;;
        esac
        _out=$(HULL_MAX_INSTRUCTIONS=2000000 "$_h" "$_app" -d "$_dsn" -- 100 2>&1) || true
        case "$_out" in
            *"DONE 100"*) ;;
            *) echo "::error audit 41 ($_label $_app): 100 SELECT 1 did not fit: $_out"; return 1 ;;
        esac
    done
    return 0
}
