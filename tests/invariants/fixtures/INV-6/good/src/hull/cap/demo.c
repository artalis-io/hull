/* INV-6 good: constants straight to stderr; app text through the escaper. */
void demo_report(const char *name, int ok)
{
    fprintf(stderr, "hull: demo starting (%d)\n", ok);
    fputs("hull: demo\n", stderr);
    fputs(ok ? "  PASS\n" : "  FAIL\n", stdout);
    snprintf(buf, sizeof buf, "%s", name);
    hl_eprintf_line("hull: demo failed for '%s'", name);
}
