# mk/vendor/quickjs.mk - QuickJS (ES2023) vendored runtime config.
# Extracted verbatim (build modularization). CFLAGS += is in-section, so this
# is included at the original position (accumulation order is load-bearing).

# ── QuickJS ──────────────────────────────────────────────────────────

QJS_DIR  := $(VENDDIR)/quickjs
QJS_SRCS := $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
            $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/libbf.c
QJS_OBJS := $(patsubst $(QJS_DIR)/%.c,$(BUILDDIR)/qjs_%.o,$(QJS_SRCS))

# QuickJS vendored-snapshot version. Bump this - and only this -
# whenever vendor/quickjs/ changes. Both the vendored QuickJS build
# (CONFIG_VERSION, used by quickjs.c) and the Hull-side bytecode /
# template caches (QJS_TAG, used to derive cache keys) read from
# this single variable, so cache invalidation is automatic on a
# QuickJS upgrade.
QJS_VERSION := 2024-01-13

# QuickJS compiled with relaxed warnings (vendored code)
QJS_CFLAGS := -std=c11 $(HL_OPT) -w -DCONFIG_VERSION=\"$(QJS_VERSION)\" \
              -DCONFIG_BIGNUM -D_GNU_SOURCE

# Hull-side code (bytecode/template caches) reads the same string via
# `include/hull/runtime/quickjs_tag.h`.
CFLAGS += -DHL_QJS_VERSION=\"$(QJS_VERSION)\"
