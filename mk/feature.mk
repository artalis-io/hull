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

# ── Canonical feature registry (single source of truth, Phase 4) ─────
# The one place the feature set is enumerated. release.yml derives its
# embedded-archive build / upload / sign lists from these (via the
# feature-embedded aggregate + the print-* targets) so adding a feature is one
# edit here, not four across Makefile / build.lua / feature.c / release.yml.
#
#   FEATURE_EMBEDDED_STEMS     archives that ship INSIDE the distributed hull
#                              (platform-domain, attested by signature.c 5c:
#                              runtime + http + wasm + sqlite + image + tls +
#                              keel + per-runtime bridges). Native-only; cosmo
#                              (fat APE) has none.
#   FEATURE_INSTALLABLE_STEMS  the --with features published as signed archives
#                              for `hull feature install` (release-domain).
FEATURE_EMBEDDED_STEMS := lua js http http-lua http-js tui-lua tui-js wasm wasm-lua wasm-js sqlite sqlite-lua sqlite-js image image-lua image-js tls keel
FEATURE_INSTALLABLE_STEMS := duckdb postgres mysql gpu tui
FEATURE_EMBEDDED_LIBS := $(addprefix $(BUILDDIR)/libhull_feature-,$(addsuffix .a,$(FEATURE_EMBEDDED_STEMS)))

.PHONY: feature-embedded print-feature-embedded-stems print-feature-embedded-libs print-feature-installable-stems
# Build every embedded feature archive (what release.yml's embedded build step runs).
feature-embedded: $(addprefix feature-,$(FEATURE_EMBEDDED_STEMS))
print-feature-embedded-stems: ; @echo $(FEATURE_EMBEDDED_STEMS)
print-feature-embedded-libs: ; @echo $(FEATURE_EMBEDDED_LIBS)
print-feature-installable-stems: ; @echo $(FEATURE_INSTALLABLE_STEMS)

# Phase 4c: fail if src/hull/commands/feature.c FEATURES[] (the `hull feature
# install/list` registry) drifts from this single source of truth. Closes the
# last of the four-place feature-list duplication; run in CI.
.PHONY: check-feature-registry
check-feature-registry: ; @sh scripts/check_feature_registry.sh "$(FEATURE_INSTALLABLE_STEMS)" "$(FEATURE_EMBEDDED_STEMS)"
