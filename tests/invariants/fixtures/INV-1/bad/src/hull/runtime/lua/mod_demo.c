/* INV-1 bad: a new binding that does syscalls and charges nothing. */
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
