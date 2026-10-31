/* INV-6 bad: an app-supplied name printed raw (ESC / OSC would reach the
 * terminal). */
void demo_report(const char *name)
{
    fprintf(stderr, "hull: demo failed for '%s'\n", name);
}
