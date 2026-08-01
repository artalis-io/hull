# mk/features/gpu.mk - wgpu-native GPU compute composable feature (hull build --with=gpu).
# Moved verbatim from the root Makefile (build modularization, Phase 1). A --with
# feature: sub-make target + archive rule, not embedded in hull, native only.

# ── GPU feature archive (composable feature: hull build --with=gpu) ──
# libhull_feature-gpu.a bundles the wgpu backend object (cap_gpu_wgpu.o, which
# defines hl_gpu_backend_wgpu) + the wgpu-native static lib into ONE archive,
# mirroring feature-duckdb. Merging into a single archive keeps the
# cap_gpu_wgpu.o <-> libwgpu_native.a refs intra-archive, so the composing link
# needs no --start-group.
#
# Unlike DuckDB (whose libs embed a clashing second mbedTLS that must be renamed),
# wgpu-native shares no symbols with Hull: the monolithic HL_ENABLE_GPU=1 build
# links wgpu + mbedTLS + SQLite + Lua together cleanly, and `nm` shows wgpu
# exports none of those names. So this archive is a straight bundle, no isolation.
# The platform link libs (-framework Metal ... on macOS / -lvulkan on Linux)
# cannot live inside a .a; they are recorded in build.lua's FEATURE_SPECS.gpu and
# emitted at the composing link. Native only (wgpu is not cosmo-compatible).
# `feature-gpu` re-invokes make with HL_ENABLE_GPU=1 so cap_gpu_wgpu.o (compiled
# as the real backend, not the base stub) + WGPU_LIB are in scope.
feature-gpu:
	$(MAKE) $(BUILDDIR)/libhull_feature-gpu.a HL_ENABLE_GPU=1
.PHONY: feature-gpu

$(eval $(call define-feature-bundle,gpu,$(BUILDDIR)/cap_gpu_wgpu.o,WGPU_LIB))
