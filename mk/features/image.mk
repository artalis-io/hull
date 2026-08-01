# mk/features/image.mk - image codec feature (docs/image_feature.md).
#
# Consolidates the image feature's definitions (previously scattered across three
# regions of the root Makefile) into one fragment: the runtime-agnostic codec
# core objects, the core + per-runtime bridge archives (via the shared
# define-feature-archive macro), the IMG_FEATURE_* registration vars, and the
# embedded_image.h embed rule. Gated by HL_ENABLE_IMAGE.
#
# Included once, before the PLATFORM_CAP_OBJS / RUNTIME_FEATURE_LIBS /
# BUILD_ASSET_OBJ aggregates that reference FEATURE_IMAGE_OBJS / IMG_FEATURE_* /
# EMBEDDED_IMAGE_H; those aggregate REFERENCES stay in the root Makefile. Needs
# mk/feature.mk (define-feature-archive), included at the top of the Makefile.
#
# IMAGE as a composable feature: the runtime-AGNOSTIC codec caps move into
# libhull_feature-image.a (with vendored stb) and compose back at `hull build`;
# the per-runtime binding (mod_image) moves into libhull_feature-image-<rt>.a.
# The base keeps image_weakstub.c so an image-less app links. Mirrors wasm.
FEATURE_IMAGE_OBJS := $(BUILDDIR)/cap_image.o $(BUILDDIR)/cap_image_stb.o
FEATURE_IMAGE_CORE := $(FEATURE_IMAGE_OBJS) $(STB_OBJ)
$(eval $(call define-feature-archive,image,$(FEATURE_IMAGE_CORE)))

# Per-runtime image-binding bridges (mod_image). Tiny (one object each);
# embedded in hull + composed for the app's runtime alongside the image core.
$(eval $(call define-feature-archive,image-lua,$(BUILDDIR)/lua_rt_mod_image.o))
$(eval $(call define-feature-archive,image-js,$(BUILDDIR)/js_mod_image.o))

# Gate the image archive vars behind HL_ENABLE_IMAGE: on the subtractive
# image-less flavor (make HL_ENABLE_IMAGE=0) cap_image.o + stb aren't built, so
# the archives can't (and needn't) build. These vars resolve empty there so
# FEATURE_ARCHIVES / RUNTIME_FEATURE_LIBS / the embed don't pull them.
ifeq ($(HL_ENABLE_IMAGE),1)
IMG_FEATURE_CORE := $(BUILDDIR)/libhull_feature-image.a
IMG_FEATURE_LUA  := $(BUILDDIR)/libhull_feature-image-lua.a
IMG_FEATURE_JS   := $(BUILDDIR)/libhull_feature-image-js.a
IMG_FEATURE_LIBS := $(IMG_FEATURE_CORE) $(IMG_FEATURE_LUA) $(IMG_FEATURE_JS)
else
IMG_FEATURE_CORE :=
IMG_FEATURE_LUA  :=
IMG_FEATURE_JS   :=
IMG_FEATURE_LIBS :=
endif

# Embed the image feature archives (docs/image_feature.md). The native base is
# image-less; the default distributed hull composes the image codec core + the
# app's runtime image bridge back whenever the app declares hull/image.
EMBEDDED_IMAGE_H := $(BUILDDIR)/embedded_image.h
$(EMBEDDED_IMAGE_H): $(BUILDDIR)/libhull_feature-image.a $(BUILDDIR)/libhull_feature-image-lua.a $(BUILDDIR)/libhull_feature-image-js.a | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i $(BUILDDIR)/libhull_feature-image.a     | sed 's/build_libhull_feature_image_a/hl_embedded_feature_image_a/g'         | $(XXD_CONST_PIPE) >> $@
	@xxd -i $(BUILDDIR)/libhull_feature-image-lua.a | sed 's/build_libhull_feature_image_lua_a/hl_embedded_feature_image_lua_a/g' | $(XXD_CONST_PIPE) >> $@
	@xxd -i $(BUILDDIR)/libhull_feature-image-js.a  | sed 's/build_libhull_feature_image_js_a/hl_embedded_feature_image_js_a/g'   | $(XXD_CONST_PIPE) >> $@
