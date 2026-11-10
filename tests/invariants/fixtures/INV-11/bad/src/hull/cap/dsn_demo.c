/* INV-11 bad: a private copy of the Windows rule (misses short names). */
int demo_confine(const char *rel)
{
    if (hl_host_is_windows() && strpbrk(rel, "\\:")) return -1;
    return 0;
}
