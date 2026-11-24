/* INV-17 bad: a binding that starts a declared command it never matched. */
static int lua_exec_run(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    return hl_exec_op_start(L, name) == 0 ? 0 : luaL_error(L, "no");
}
