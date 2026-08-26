# mk/features/runtime.mk - Lua + QuickJS runtimes as composable features.
#
# Both runtime archives (libhull_feature-{lua,js}.a: the VM + runtime objects +
# manifest extractor + stdlib VFS) - a single fragment because they are
# symmetric and share the EMBEDDED_RUNTIME_H embed (which embeds both, exactly
# one composed per app from the entry extension). Moved verbatim from two
# regions of the root Makefile (build modularization, Phase 1). Included after
# mk/features/http.mk: FEATURE_LUA/JS_OBJS filter out FEATURE_HTTP_LUA/JS_OBJS.
#
# Stays in the root Makefile: FEATURE_ARCHIVES + RUNTIME_FEATURE_LIBS (cross-
# cutting aggregates) and the shared CFLAGS/BUILD_ASSET_OBJ lines. (Phase 2
# macro-ifies the archive rules.)

# libhull_feature-lua.a / -js.a: a runtime as a composable feature archive.
# Bundles the runtime objects, its vendored VM, its manifest extractor, and its
# stdlib VFS array (hl_stdlib_<rt>_entries). The tui bridge (mod_tui) is excluded
# - it belongs to libhull_feature-tui.a. These build additively (base still dual,
# objects already compiled); the base is runtime-less and force-loads them into
# hull. Whole-archive at compose (no single anchor symbol).
# Exclude the tui bridge (-> libhull_feature-tui.a) and the Lua tool VM bindings
# (lua_rt_mod_tool.o: tool.extract_manifest_js etc., toolchain-only; whole-
# archiving the runtime must not force-load them - they pull the JS manifest
# extractor the runtime-less base no longer carries). hull links them directly.

FEATURE_LUA_OBJS := $(filter-out $(BUILDDIR)/lua_rt_mod_tui.o $(BUILDDIR)/lua_rt_mod_tool.o $(BUILDDIR)/lua_rt_mod_compute.o $(BUILDDIR)/lua_rt_mod_db_udf.o $(BUILDDIR)/lua_rt_mod_image.o $(FEATURE_HTTP_LUA_OBJS),$(LUA_RT_OBJS)) \
                    $(LUA_OBJS) $(BUILDDIR)/manifest_lua.o $(STDLIB_LUA_REGISTRY_O)
FEATURE_JS_OBJS  := $(filter-out $(BUILDDIR)/js_mod_tui.o $(BUILDDIR)/js_mod_compute.o $(BUILDDIR)/js_mod_db_udf.o $(BUILDDIR)/js_mod_image.o $(FEATURE_HTTP_JS_OBJS),$(JS_RT_OBJS)) \
                    $(QJS_OBJS) $(BUILDDIR)/manifest_js.o $(STDLIB_JS_REGISTRY_O)

$(eval $(call define-feature-archive,lua,$(FEATURE_LUA_OBJS)))

$(eval $(call define-feature-archive,js,$(FEATURE_JS_OBJS)))

# Embed both runtime feature archives so the runtime-less native base composes
# one at build time with no `hull feature install` (the runtime is mandatory).
EMBEDDED_RUNTIME_H := $(BUILDDIR)/embedded_runtime.h
$(EMBEDDED_RUNTIME_H): $(BUILDDIR)/libhull_feature-lua.a $(BUILDDIR)/libhull_feature-js.a | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i $(BUILDDIR)/libhull_feature-lua.a | sed 's/build_libhull_feature_lua_a/hl_embedded_feature_lua_a/g' | $(XXD_CONST_PIPE) >> $@
	@xxd -i $(BUILDDIR)/libhull_feature-js.a  | sed 's/build_libhull_feature_js_a/hl_embedded_feature_js_a/g'   | $(XXD_CONST_PIPE) >> $@
