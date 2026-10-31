/* INV-2 good: a binding goes through hl_cap_db_*; a comment that names
 * sqlite3_exec() is not a call, nor is a string "sqlite3_exec(". */
static int l_demo(lua_State *L)
{
    log_info("not sqlite3_exec(db) - a string");
    return hl_cap_db_exec(NULL, "SELECT 1", NULL, 0) == 0;
}
