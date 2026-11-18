/* INV-14 bad: a development loader that reads the file a generation
 * entry would shadow. */
int demo_load(HlLua *lua, const char *rel)
{
    char path[256];
    if (hl_vfs_embedded(lua->base.app_vfs))
        return -1;
    snprintf(path, sizeof path, "%s/%s", lua->app_dir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fclose(f);
    return 0;
}
