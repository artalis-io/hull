# mk/vendor/log.mk - rxi/log.c vendored config (extracted verbatim; DEPFLAGS applied by
# the shared block in the root Makefile after all vendor includes).

# ── rxi/log.c ─────────────────────────────────────────────────────────

LOG_DIR    := $(VENDDIR)/log.c
LOG_OBJ    := $(BUILDDIR)/log.o
LOG_CFLAGS := -std=c11 -O2 -w -DLOG_USE_COLOR
