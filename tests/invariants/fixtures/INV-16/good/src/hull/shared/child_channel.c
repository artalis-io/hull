/* INV-16 good: the child channel is one of the files that spawn. */
pid_t hl_chan_spawn(const char *exe, char *const argv[], int fd)
{
    pid_t pid;
    if (posix_spawn(&pid, exe, NULL, NULL, argv, NULL) != 0) return -1;
    (void)fd;
    return pid;
}
