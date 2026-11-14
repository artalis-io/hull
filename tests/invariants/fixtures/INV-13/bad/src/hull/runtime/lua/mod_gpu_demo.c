/* INV-13 bad: a binding takes the GPU device mutex with a plain lock, so it
 * blocks behind a gpu.async job past the run's deadline (audit 46). */
size_t demo_copy_bytes(HlGpuCtx *ctx, int idx)
{
    HlGpuDevice *dev = &ctx->devices[idx];
    pthread_mutex_lock(&dev->mutex);
    size_t n = dev->buffer_count;
    pthread_mutex_unlock(&dev->mutex);
    return n;
}
