/* INV-12 bad: an entry point initialises a GPU context itself, with no
 * shader policy before the app's top level runs. */
int demo_open(HlGpuCtx *ctx, const HlGpuBackend *be)
{
    return hl_cap_gpu_init(ctx, be);
}
