# mk/vendor/keel.mk - Keel HTTP server library (git submodule) config.
# Extracted verbatim; the KEEL_EXTRA_* recipe forward-refs (HL_LTO/CFI) are
# deferred to recipe-run time. Included at the original position.

# ── Keel (external library) ─────────────────────────────────────────

# Keel is included as a git submodule in vendor/keel. Dropped from the
# link only when both HTTP halves are off - Keel ships the HTTP client
# (used by HL_ENABLE_HTTP_CLIENT=1) and the HTTP server (used by
# HL_ENABLE_HTTP_SERVER=1) together; the linker dead-strips the half
# that isn't referenced.
KEEL_DIR   ?= $(VENDDIR)/keel
KEEL_INC   := $(KEEL_DIR)/include
ifeq ($(HL_LINK_TLS),0)
KEEL_LIB   :=
else
KEEL_LIB   := $(KEEL_DIR)/libkeel.a
endif

# Build Keel with mbedTLS backend
# Keel now detects the cosmo toolchain natively from CC and handles
# poll backend, .aarch64/ archive creation, etc.
MINIZ_DIR  := $(VENDDIR)/miniz

ifneq ($(KEEL_LIB),)
# NOTE: the per-request sealed-request snapshot (mprotect-RO, -DKEEL_SEAL_REQUEST)
# was a Keel v2.3.1-v2.7.1 feature that Keel REMOVED in v2.8.0; Keel 3.x reads
# KlHttpRequest fields directly (concrete struct) and no longer vendors
# sh_seal_arena. Hull therefore no longer passes KEEL_SEAL_REQUEST. See
# docs/security.md § 4e for the security-posture note and the possible
# Hull-side re-implementation (via hl_seal_arena) as a future follow-up.

$(KEEL_LIB): $(MBEDTLS_OBJS)
	$(MAKE) -C $(KEEL_DIR) CC=$(CC) AR=$(AR) \
		KEEL_TLS=mbedtls MBEDTLS_DIR=$(abspath $(MBEDTLS_DIR)) MBEDTLS_CONFIG_FILE=hull_config.h \
		KEEL_COMPRESS=miniz MINIZ_DIR=$(CURDIR)/$(MINIZ_DIR) \
		KEEL_EXTRA_CFLAGS="$(HL_LTO_CFLAG) $(if $(HL_CFI_CFLAG),$(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit)" \
		KEEL_EXTRA_LDFLAGS="$(HL_LTO_CFLAG) $(if $(HL_CFI_CFLAG),$(HL_CFI_CFLAG) -fsplit-lto-unit)"
endif
