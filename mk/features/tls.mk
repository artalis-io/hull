# mk/features/tls.mk - mbedTLS/TLS composable feature (docs/tls_feature.md).
#
# The tls FEATURE ARCHIVE (libhull_feature-tls.a) + its embed. Moved verbatim
# from the root Makefile (build modularization, Phase 1), included at the
# original position (after AR_FEATURE_LIB / MBEDTLS_OBJS / KEEL_LIB are set).
#
# NOTE: the base-object gating for TLS (MBEDTLS_OBJS, TLS_CLIENT_OBJ /
# TLS_TRANSPORT_OBJ, FEATURE_TLS_CAP_OBJS) stays in the root Makefile - it
# controls what the BASE links (referenced in ~10 base/test link lines), a
# base concern, not the feature archive. This fragment owns only how the
# feature archive is built + embedded. (Phase 2 macro-ifies the archive rule.)

# libhull_feature-tls.a: the mbedTLS transport + crypto backends as a composable
# feature (docs/tls_feature.md, a2). Bundles the mbedTLS-consuming TUs -- the two
# crypto mbedTLS backends (strong overrides of the weak hl_crypto_*_active_backend
# hooks), the outbound TLS client (smtp/pg/mysql sslmode), the serve TLS ctx setup
# (a1), and the vendored mbedTLS (~1 MB) -- so a TLS-less app-build base drops them
# and composes them back when the app needs TLS. References Keel's tls_mbedtls.o
# (resolved from the base's KEEL_LIB at compose) + base crypto/vfs symbols, so the
# compose whole-archives it inside a --start-group. Built at HL_LINK_TLS=1 (the
# default HTTP build already compiles all members).
# Keel's TLS session object (kl_tls_mbedtls_*). It normally rides inside the
# platform lib's Keel merge, but a TLS-less base excludes it (see the keel-merge
# rule); so the feature archive carries its own copy, extracted from KEEL_LIB and
# renamed (keel_tls_mbedtls.o) to avoid a member-name clash with the crypto TUs.
$(BUILDDIR)/keel_tls_mbedtls.o: $(KEEL_LIB) | $(BUILDDIR)
	@cd $(BUILDDIR) && $(AR) x $(CURDIR)/$(KEEL_LIB) tls_mbedtls.o && mv -f tls_mbedtls.o keel_tls_mbedtls.o
FEATURE_TLS_OBJS := $(BUILDDIR)/cap_crypto_hmac_mbedtls.o $(BUILDDIR)/cap_crypto_asym_mbedtls.o \
                    $(BUILDDIR)/tls_client.o $(BUILDDIR)/tls_transport.o \
                    $(BUILDDIR)/keel_tls_mbedtls.o $(MBEDTLS_OBJS)
feature-tls: $(BUILDDIR)/libhull_feature-tls.a
.PHONY: feature-tls
$(BUILDDIR)/libhull_feature-tls.a: $(FEATURE_TLS_OBJS) | $(BUILDDIR)
	$(call AR_FEATURE_LIB,$(FEATURE_TLS_OBJS))

# TLS feature archive embed (a2, HL_APP_BASE_TLSLESS=1): when the app-build base
# is TLS-less, embed libhull_feature-tls.a (mbedTLS + the crypto/tls TUs) so an
# HTTPS / net-DB app auto-composes it with no `hull feature install`. Mirrors the
# SQLite engine embed. Only pulled when HL_APP_BASE_TLSLESS=1.
ifeq ($(HL_APP_BASE_TLSLESS),1)
EMBEDDED_TLS_H := $(BUILDDIR)/embedded_tls.h
$(EMBEDDED_TLS_H): $(BUILDDIR)/libhull_feature-tls.a | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i $(BUILDDIR)/libhull_feature-tls.a | sed 's/build_libhull_feature_tls_a/hl_embedded_feature_tls_a/g' | $(XXD_CONST_PIPE) >> $@
CFLAGS += -DHL_BUILD_EMBEDDED_TLS
$(BUILD_ASSET_OBJ): $(EMBEDDED_TLS_H)
endif
