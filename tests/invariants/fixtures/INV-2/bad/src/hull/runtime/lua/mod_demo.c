/* INV-2 bad: a binding calling the engine directly. */
static int l_demo(lua_State *L)
{
    sqlite3 *db = get_db(L);
    return sqlite3_exec(db, "SELECT 1", NULL, NULL, NULL) == 0;
}
