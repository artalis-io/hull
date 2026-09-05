# mk/vendor/tweetnacl.mk - TweetNaCl (Ed25519) vendored config (extracted verbatim; DEPFLAGS applied by
# the shared block in the root Makefile after all vendor includes).

# ── TweetNaCl (Ed25519 signatures) ─────────────────────────────────

TWEETNACL_DIR    := $(VENDDIR)/tweetnacl
TWEETNACL_OBJ    := $(BUILDDIR)/tweetnacl.o
TWEETNACL_CFLAGS := -std=c11 $(HL_OPT) -w
