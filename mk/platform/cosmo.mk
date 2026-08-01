# mk/platform/cosmo.mk - Cosmopolitan (fat APE) platform policy.
#
# No OS-global Makefile content today: pledge/unveil are built into cosmo (no
# polyfill), and the poll backend + fat-APE archive conventions (.aarch64/) live
# with their concern (Keel's own Makefile, mk/libhull.mk's LIBHULL_COSMO_FAT, the
# base sub-builds). This file is the seam for any future cosmo-global policy.
