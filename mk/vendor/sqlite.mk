# mk/vendor/sqlite.mk - SQLite amalgamation vendored config (extracted verbatim; DEPFLAGS applied by
# the shared block in the root Makefile after all vendor includes).

# ── SQLite (vendored amalgamation) ─────────────────────────────────

SQLITE_DIR    := $(VENDDIR)/sqlite
ifeq ($(HL_ENABLE_SQLITE),1)
SQLITE_OBJ    := $(BUILDDIR)/sqlite3.o
else
SQLITE_OBJ    :=
endif
SQLITE_CFLAGS := -std=c11 -O2 -w -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5
