/*
 * entry.c — Thin main() trampoline for hull binary
 *
 * The actual logic lives in hull_main() (main.c), which is also
 * exported from libhull_platform.a so that `hull build` can link
 * standalone app binaries against it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

extern int hull_main(int argc, char **argv);

int main(int argc, char **argv)
{
    return hull_main(argc, argv);
}
