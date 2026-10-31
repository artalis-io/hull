/* INV-9 bad: no resumer - the closed thread gets no C-call depth and its
 * __close handlers' work goes uncharged. */
int demo_resume(lua_State *L, lua_State *co)
{
    int nres = 0;
    int st = lua_resume(co, NULL, 0, &nres);
    if (st != LUA_OK) (void)lua_closethread(co, NULL);
    return st;
}
