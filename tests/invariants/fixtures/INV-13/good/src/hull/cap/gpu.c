/* INV-13 good: the device mutex is taken through dev_lock, whose sliced
 * wait asks the run; other mutexes are not the rule's concern. */
static int dev_lock(HlGpuDevice *dev)
{
    if (pthread_mutex_trylock(&dev->mutex) == 0) return 0;
    return -1;
}

int demo_copy_bytes(HlGpuDevice *dev, CompileJob *job)
{
    if (dev_lock(dev) != 0)
        return -1;
    pthread_mutex_lock(&job->mu);
    pthread_mutex_unlock(&job->mu);
    pthread_mutex_unlock(&dev->mutex);
    return 0;
}
