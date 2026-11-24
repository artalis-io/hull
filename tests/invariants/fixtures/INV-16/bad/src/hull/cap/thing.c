/* INV-16 bad: a capability that forks and execs a program of its own - a
 * new way out of the app sandbox. */
int thing_run(const char *path)
{
    pid_t p = fork();
    if (p == 0) {
        execv(path, NULL);
        _exit(127);
    }
    return p < 0 ? -1 : 0;
}
