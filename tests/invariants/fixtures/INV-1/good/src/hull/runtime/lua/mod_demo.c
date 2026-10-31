/* INV-1 good: every binding charges - directly, through a helper, or
 * through a macro whose body charges. */
#define DEMO_CHARGE(L, n) lua_hlwork((L), (n))

static void charge_bytes(lua_State *L, size_t n)
{
    lua_hlwork(L, n / 8);
}

static int l_direct(lua_State *L)
{
    hl_lua_budget_charge(L, 16);
    return 0;
}

static int l_helper(lua_State *L)
{
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    charge_bytes(L, n);
    (void)s;
    return 0;
}

static int l_macro(lua_State *L)
{
    DEMO_CHARGE(L, 4);
    return 0;
}

static const luaL_Reg demo_funcs[] = {
    {"direct", l_direct},
    {"helper", l_helper},
    {"macro",  l_macro},
    {NULL, NULL}
};
