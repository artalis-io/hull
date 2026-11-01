/* INV-1 bad: a module registered with the raw luaL_newlib, so its
 * bindings skip the binding dispatcher (no measured-time charge). */
static int l_unlink(lua_State *L)
{
    const char *p = luaL_checkstring(L, 1);
    lua_pushboolean(L, hl_cap_fs_delete(p) == 0);
    return 1;
}

static const luaL_Reg demo_funcs[] = {
    {"unlink", l_unlink},
    {NULL, NULL}
};

int luaopen_demo(lua_State *L)
{
    luaL_newlib(L, demo_funcs);
    return 1;
}
