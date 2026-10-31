/* INV-9 good: the resumer is passed, and the status goes through
 * hl_lua_resume_status. */
int demo_resume(lua_State *L, lua_State *co)
{
    int nres = 0;
    int st = lua_resume(co, L, 0, &nres);
    st = hl_lua_resume_status(co, L, st);
    if (st != LUA_OK) (void)lua_closethread(co, L);
    return st;
}
