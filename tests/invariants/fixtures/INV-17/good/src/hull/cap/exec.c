/* INV-17 good: the one matcher, called only from hl_exec_check_call. */
int hl_exec_slot_match(const HlExecSlot *slot, const char *v, size_t n)
{
    return slot && v && n ? 0 : -1;
}

int hl_exec_check_call(const uint8_t *policy, const char *name, const char *v, size_t n)
{
    HlExecSlot s = slot_of(policy, name);
    return hl_exec_slot_match(&s, v, n);
}
