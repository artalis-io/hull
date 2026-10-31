/* INV-5 bad: a loader that reads beside a built binary. */
int demo_load(HlLua *lua, const char *rel)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", lua->app_dir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fclose(f);
    return 0;
}
