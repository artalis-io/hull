# mk/features/wasm.mk - WASM/WAMR composable feature (docs/wasm_feature.md).
#
# The wasm FEATURE ARCHIVE (libhull_feature-wasm.a: cap_wasm* + worker_wasm +
# vendored WAMR) + the per-runtime compute bridges (mod_compute) + the embed.
# Moved verbatim from three regions of the root Makefile (build modularization,
# Phase 1). Included at the FEATURE_*_OBJS cluster position, before the
# PLATFORM_CAP_OBJS / RUNTIME_FEATURE_LIBS / BUILD_ASSET_OBJ aggregates below.
#
# The base-object gating (WAMR_OBJS, WORKER_WASM_OBJ, WASM_WEAKSTUB_OBJ) stays
# in the root Makefile - it controls what the base/hull-tool links (hull + test
# link lines), a base concern. (Phase 2 macro-ifies the archive rules.)

# WASM as a composable feature (docs/wasm_feature.md, Phase 1). The runtime-
# AGNOSTIC wasm caps move into libhull_feature-wasm.a (with worker_wasm + the
# vendored WAMR objects) and compose back at `hull build`; the per-runtime
# compute BINDING (mod_compute) moves into libhull_feature-wasm-<rt>.a. The base
# keeps the wasm_weakstub.c weak defaults (Phase 0) so a compute-less app links.
FEATURE_WASM_OBJS := $(BUILDDIR)/cap_wasm.o $(BUILDDIR)/cap_wasm_buffer.o \
                     $(BUILDDIR)/cap_wasm_data.o $(BUILDDIR)/cap_wasm_stream.o

# libhull_feature-wasm.a: the runtime-agnostic WASM CORE (docs/wasm_feature.md,
# Phase 1). Bundles the wasm caps + worker_wasm + the vendored WAMR objects
# (the ~256 KB that leaves the base). Composed back at `hull build` and embedded
# in hull (embedded_wasm.h). The per-runtime compute binding (mod_compute) is a
# separate archive below, like the http web bindings.
FEATURE_WASM_CORE := $(FEATURE_WASM_OBJS) $(WORKER_WASM_OBJ) $(WAMR_OBJS)
$(eval $(call define-feature-archive,wasm,$(FEATURE_WASM_CORE)))

# Per-runtime compute-binding bridges (mod_compute). Tiny (one object each);
# embedded in hull + composed for the app's runtime alongside the wasm core.
$(eval $(call define-feature-archive,wasm-lua,$(BUILDDIR)/lua_rt_mod_compute.o))

$(eval $(call define-feature-archive,wasm-js,$(BUILDDIR)/js_mod_compute.o))

# Embed the WASM feature archives too (docs/wasm_feature.md, Phase 1). The native
# base is compute-less; the default distributed hull composes the wasm core + the
# app's runtime compute bridge back for every full-flavor app with no install.
EMBEDDED_WASM_H := $(BUILDDIR)/embedded_wasm.h
$(EMBEDDED_WASM_H): $(BUILDDIR)/libhull_feature-wasm.a $(BUILDDIR)/libhull_feature-wasm-lua.a $(BUILDDIR)/libhull_feature-wasm-js.a | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i $(BUILDDIR)/libhull_feature-wasm.a     | sed 's/build_libhull_feature_wasm_a/hl_embedded_feature_wasm_a/g'         | $(XXD_CONST_PIPE) >> $@
	@xxd -i $(BUILDDIR)/libhull_feature-wasm-lua.a | sed 's/build_libhull_feature_wasm_lua_a/hl_embedded_feature_wasm_lua_a/g' | $(XXD_CONST_PIPE) >> $@
	@xxd -i $(BUILDDIR)/libhull_feature-wasm-js.a  | sed 's/build_libhull_feature_wasm_js_a/hl_embedded_feature_wasm_js_a/g'   | $(XXD_CONST_PIPE) >> $@
