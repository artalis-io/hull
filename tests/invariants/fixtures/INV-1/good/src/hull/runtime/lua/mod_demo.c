/* INV-1 good: every binding is registered with the binding dispatcher -
 * a luaL_Reg table through hl_lua_newlib / hl_lua_setfuncs, a table's rows
 * pushed in a loop with hl_lua_pushcclosure, or one function with
 * hl_lua_pushcfunction. None needs an explicit charge to pass. */
static int l_one(lua_State *L)   { (void)L; return 0; }
static int l_two(lua_State *L)   { (void)L; return 0; }
static int l_three(lua_State *L) { (void)L; return 0; }
static int l_four(lua_State *L)  { (void)L; return 0; }

static const luaL_Reg demo_funcs[] = {
    {"one", l_one},
    {"two", l_two},
    {NULL, NULL}
};

static const luaL_Reg demo_methods[] = {
    {"three", l_three},
    {NULL, NULL}
};

static void push_rows(lua_State *L, const luaL_Reg *funcs)
{
    for (const luaL_Reg *m = funcs; m->name; m++) {
        hl_lua_pushcclosure(L, m->func, 0);
        lua_setfield(L, -2, m->name);
    }
}

int luaopen_demo(lua_State *L)
{
    hl_lua_newlib(L, demo_funcs);
    push_rows(L, demo_methods);
    hl_lua_pushcfunction(L, l_four);
    lua_setfield(L, -2, "four");
    return 1;
}
