# mk/vendor/sh_json.mk - sh_json vendored config (extracted verbatim; DEPFLAGS applied by
# the shared block in the root Makefile after all vendor includes).

# ── sh_json (vendored from otto) ──────────────────────────────────────

SH_JSON_DIR    := $(VENDDIR)/sh_json
SH_JSON_OBJ    := $(BUILDDIR)/sh_json.o
SH_JSON_CFLAGS := -std=c11 $(HL_OPT) -w
