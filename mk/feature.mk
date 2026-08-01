# mk/feature.mk - reusable macros for composable-feature archive rules.
#
# Part of the build modularization (docs/build_modularization.md). Emits the
# mechanical per-feature archive rule + `feature-<stem>` phony, built via the
# shared AR_FEATURE_LIB macro (which honours TRUST_FEATURE_LIBS at release
# stage 3 - trust a signed pre-built artifact instead of rebuilding). This is
# the scaffolding around AR_FEATURE_LIB, not a re-implementation of the ar step.
#
#   $(call define-feature-archive,STEM,OBJS)
#     STEM  archive stem: "image", "image-lua", "http", "sqlite-js", ...
#     OBJS  the object list (already-expanded; the FEATURE_*_ vars are :=)
#
# Invoke via $(eval):
#   $(eval $(call define-feature-archive,image,$(FEATURE_IMAGE_CORE)))
#
# The generated rule is behaviour-identical to the hand-written form
#   feature-STEM: $(BUILDDIR)/libhull_feature-STEM.a
#   .PHONY: feature-STEM
#   $(BUILDDIR)/libhull_feature-STEM.a: OBJS | $(BUILDDIR)
#           $(call AR_FEATURE_LIB,OBJS)
# proven on the `image` feature (core + both runtime bridges) as the Phase 0
# PoC (nm object-membership identical before/after).
define define-feature-archive
feature-$(1): $$(BUILDDIR)/libhull_feature-$(1).a
.PHONY: feature-$(1)
$$(BUILDDIR)/libhull_feature-$(1).a: $(2) | $$(BUILDDIR)
	$$(call AR_FEATURE_LIB,$(2))
endef
