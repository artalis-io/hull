# mk/vendor/keel.mk - Keel HTTP server library (git submodule) config.
# Extracted verbatim; the KEEL_EXTRA_* recipe forward-refs (HL_LTO/CFI) are
# deferred to recipe-run time. Included at the original position.

# ── Keel (external library) ─────────────────────────────────────────

# Keel is included as a git submodule in vendor/keel. Dropped from the
# link only when both HTTP halves are off — Keel ships the HTTP client
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
# Per-request sealed request snapshot (Keel v2.7.0+).  Defends parsed
# request fields (method, path, headers, route params, content_length,
# chunked, version) against in-process memory corruption that lands
# on the request struct.  Bytes are copied into a per-connection
# sh_seal_arena between routing and the first user-code dispatch,
# then mprotect(PROT_READ).  Per-request overhead: ~3-15us (one
# struct copy + per-header bytes copy + two mprotect calls).
# Negligible on real Hull workloads (DB / template / JSON dominates);
# visible on Keel microbenches.  See vendor/keel/include/keel/request.h
# and docs/security.md § 4e.
#
# Default ON in Hull.  Opt out with KEEL_DISABLE_SEAL_REQUEST=1
# for benchmarking or to debug a regression that bisects to the seal.
KEEL_SEAL_REQUEST_FLAG := $(if $(KEEL_DISABLE_SEAL_REQUEST),,-DKEEL_SEAL_REQUEST=1)

# Hull's own TUs include keel/request.h and call the inline accessors
# (kl_request_method, kl_request_header_at, ...).  The flag changes
# struct KlRequest's layout (adds the `sealed` pointer) and the accessor
# bodies, so Hull MUST be compiled with the SAME define as libkeel.a or
# the offsets diverge and the accessors dereference a never-set field ->
# segfault.  Keep this in lockstep with KEEL_EXTRA_CFLAGS below.
CFLAGS += $(KEEL_SEAL_REQUEST_FLAG)

$(KEEL_LIB): $(MBEDTLS_OBJS)
	$(MAKE) -C $(KEEL_DIR) CC=$(CC) AR=$(AR) \
		KEEL_TLS=mbedtls MBEDTLS_CONFIG_FILE=hull_config.h \
		KEEL_COMPRESS=miniz MINIZ_DIR=$(CURDIR)/$(MINIZ_DIR) \
		KEEL_EXTRA_CFLAGS="$(HL_LTO_CFLAG) $(if $(HL_CFI_CFLAG),$(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit) $(KEEL_SEAL_REQUEST_FLAG)" \
		KEEL_EXTRA_LDFLAGS="$(HL_LTO_CFLAG) $(if $(HL_CFI_CFLAG),$(HL_CFI_CFLAG) -fsplit-lto-unit)"
endif
