# mk/vendor/mbedtls.mk - mbedTLS vendored TLS/crypto config + mbed_%.o rule.
# Extracted verbatim; MBEDTLS_CFLAGS gets DEPFLAGS from the shared block after
# all vendor includes. Included at the original position.

# ── mbedTLS (vendored) ─────────────────────────────────────────────
#
# mbedTLS is used by both halves of the HTTP stack:
#   - HTTP server (HL_ENABLE_HTTP_SERVER=1): TLS termination via Keel's
#     KlTlsCtx in serve.c.
#   - HTTP client (HL_ENABLE_HTTP_CLIENT=1): HTTPS for http.fetch +
#     STARTTLS for SMTP send (cap/smtp.c).
# Dropped from the link only when BOTH are off (pure-compute build).

MBEDTLS_DIR    := $(VENDDIR)/mbedtls
MBEDTLS_SRCS   := $(wildcard $(MBEDTLS_DIR)/library/*.c)
ifeq ($(HL_LINK_TLS),0)
MBEDTLS_OBJS   :=
else ifeq ($(HL_TLS_FEATURE),1)
MBEDTLS_OBJS   :=   # composed from libhull_feature-tls.a; not in the base object set
else
MBEDTLS_OBJS   := $(patsubst $(MBEDTLS_DIR)/library/%.c,$(BUILDDIR)/mbed_%.o,$(MBEDTLS_SRCS))
endif
MBEDTLS_CFLAGS := -std=c11 $(HL_OPT) -w \
	-I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library -I$(MBEDTLS_DIR) \
	-DMBEDTLS_CONFIG_FILE='"hull_config.h"'

$(BUILDDIR)/mbed_%.o: $(MBEDTLS_DIR)/library/%.c | $(BUILDDIR)
	$(CC) $(MBEDTLS_CFLAGS) -c -o $@ $<
