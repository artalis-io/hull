/* INV-16 bad: the same through a raw syscall number. */
static int lua_thing(lua_State *L)
{
    (void)L;
    return (int)syscall(__NR_execveat, 3, "", NULL, NULL, 0x1000);
}
