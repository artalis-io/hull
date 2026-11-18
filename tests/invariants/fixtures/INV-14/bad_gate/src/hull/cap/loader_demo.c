/* INV-14 bad: a capability-layer loader (it re-checks what it read against
 * the signature) that never asks the generation overlay. */
int demo_load(const HlVfs *vfs, const char *rel, char *buf, size_t n)
{
    if (hl_vfs_embedded(vfs))
        return -1;
    return hl_vfs_disk_gate_check(rel, buf, n);
}
