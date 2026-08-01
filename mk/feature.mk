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

# $(call define-feature-bundle,STEM,FIRST_OBJ,ARCHIVES_VAR): archive rule for a
# vendored-engine --with feature (duckdb, gpu). Bundles FIRST_OBJ plus the
# objects extracted from each vendored static lib named in the ARCHIVES_VAR
# variable into libhull_feature-STEM.a. Each source archive is extracted into
# its own subdir so same-named members from different archives coexist (ar
# tolerates duplicate member names; ld pulls whichever resolves a needed symbol
# via the index), which keeps the engine's circular refs intra-archive (no
# --start-group at compose). Does NOT emit a feature-STEM phony: those are
# sub-make targets (`$(MAKE) ... HL_ENABLE_<X>=1`) and stay hand-written in the
# fragment. Raw ar by design - these are --with / release-domain assets
# (hull.sha256), not the platform-key trust-embedded set, so AR_FEATURE_LIB /
# TRUST_FEATURE_LIBS do not apply.
#
# ARCHIVES_VAR is the NAME of the variable (e.g. DUCKDB_ARCHIVES), not its value:
# it is dereferenced with $($(3)) so the archive list expands at recipe-run time
# (deferred), byte-identical to the hand-written duckdb/gpu recipes - it is only
# populated in the HL_ENABLE_<X>=1 sub-make.
define define-feature-bundle
$$(BUILDDIR)/libhull_feature-$(1).a: $(2) $$($(3)) | $$(BUILDDIR)
	@rm -f $$@
	$$(AR) rcs $$@ $(2)
	@tmproot=$$$$(mktemp -d); n=0; for a in $$($(3)); do \
		mkdir -p $$$$tmproot/$$$$n && ( cd $$$$tmproot/$$$$n && $$(AR) x $$(CURDIR)/$$$$a ) && \
		$$(AR) rcs $$(CURDIR)/$$@ $$$$tmproot/$$$$n/*.o && n=$$$$((n+1)); \
	done; rm -rf $$$$tmproot
	@echo "built $$@ ($$$$(du -h $$@ | cut -f1))"
endef
