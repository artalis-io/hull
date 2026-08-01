# mk/vendor/sh_arena.mk - sh_arena vendored config (extracted verbatim; DEPFLAGS applied by
# the shared block in the root Makefile after all vendor includes).

# ── sh_arena (vendored from otto) ────────────────────────────────────

SH_ARENA_DIR    := $(VENDDIR)/sh_arena
SH_ARENA_OBJ    := $(BUILDDIR)/sh_arena.o
SH_ARENA_CFLAGS := -std=c11 -O2 -w
# Under ASan (`make debug`) instrument the arena TU so its manual ASan
# poison/unpoison calls activate (dangling-into-arena reads in Hull code
# become hard ASan errors). Other vendored TUs intentionally stay
# uninstrumented; this one is tiny and the integration needs it ASan-aware.
ifdef DEBUG
SH_ARENA_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif
