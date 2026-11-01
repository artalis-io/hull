/* INV-1 bad: a binding pushed with a bare lua_pushcfunction - it charges
 * explicitly, but it is not registered with the binding dispatcher. */
static int l_spin(lua_State *L)
{
    hl_lua_budget_charge(L, 16);
    return 0;
}

void demo_register(lua_State *L)
{
    lua_pushcfunction(L, l_spin);
    lua_setglobal(L, "spin");
}
