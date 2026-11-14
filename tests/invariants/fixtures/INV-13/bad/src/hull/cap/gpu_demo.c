/* INV-13 bad: in a GPU file a device mutex reached by any name - another
 * variable, an array element, a timed lock - is still taken outside dev_lock. */
size_t demo_count(HlGpuCtx *ctx, int idx, HlGpuDevice *gdev)
{
    pthread_mutex_lock(&ctx->devices[idx].mutex);
    size_t n = ctx->devices[idx].buffer_count;
    pthread_mutex_unlock(&ctx->devices[idx].mutex);
    struct timespec ts = { 0, 0 };
    if (pthread_mutex_timedlock(&gdev->mutex, &ts) == 0)
        pthread_mutex_unlock(&gdev->mutex);
    return n;
}
