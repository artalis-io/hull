/* INV-11 good: the app path goes through the shared Windows predicate. */
int demo_confine(const char *rel)
{
    if (hl_host_path_unsafe_on_windows(rel)) return -1;
    return hl_host_is_windows() ? 1 : 0;
}
