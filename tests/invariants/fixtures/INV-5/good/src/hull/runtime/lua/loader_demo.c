/* INV-5 good: the disk path is taken only when nothing is embedded. */
static int read_disk(HlLua *lua, const char *rel, char *buf, size_t n)
{
    return hl_vfs_disk_gate_check(rel, buf, n);
}

int demo_load(HlLua *lua, const char *rel)
{
    char buf[64];
    if (hl_vfs_embedded(lua->base.app_vfs))
        return -1;
    return read_disk(lua, rel, buf, sizeof buf);
}
