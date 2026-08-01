# mk/platform/darwin.mk - native macOS platform policy.
#
# No OS-global Makefile content today: the sandbox is seatbelt (applied in
# sandbox.c, no compiled polyfill), the link is the universal -lm -lpthread, and
# macOS-specific vendor wiring (wgpu Metal frameworks, WAMR darwin platform_init)
# lives with its vendor in mk/vendor/*. This file is the seam for any future
# macOS-global policy (ld64 link flags, notarization, ...).
