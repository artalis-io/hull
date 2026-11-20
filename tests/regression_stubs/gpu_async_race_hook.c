/* Pre-fix stand-in for hl_gpu_test_set_async_race_hook (audit 49): the
 * pre-fix gpu.c has no race hook, so this records nothing.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
void hl_gpu_test_set_async_race_hook(void (*fn)(int point));

void hl_gpu_test_set_async_race_hook(void (*fn)(int point))
{
    (void)fn;
}
