/* INV-12 good: the entry point opens the context through the shared
 * helper, which sets the shader policy. */
HlGpuCtx *demo_open(const HlVfs *vfs)
{
    return hl_gpu_ctx_open(-1, vfs);
}
