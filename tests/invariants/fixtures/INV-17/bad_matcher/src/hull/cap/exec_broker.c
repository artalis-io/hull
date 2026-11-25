/* INV-17 bad: the broker matches slot by slot itself (a second matcher)
 * and forks without hl_exec_check_call. */
static void run_call(const HlExecSlot *slots, const char *const *args, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (hl_exec_slot_match(&slots[i], args[i], strlen(args[i])) != 0) return;
    if (fork() == 0) _exit(0);
}
