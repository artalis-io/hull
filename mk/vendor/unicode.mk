# mk/vendor/unicode.mk - Unicode cell-width tables vendored config (extracted verbatim; DEPFLAGS applied by
# the shared block in the root Makefile after all vendor includes).

# ── Unicode tables (TUI cell-width lookup) ──────────────────────────
#
# vendor/unicode/eaw.h is checked in (~28 KB) and included by
# src/hull/cap/tui_width.c via "unicode/eaw.h". The Unicode data
# files (EastAsianWidth.txt + UnicodeData.txt) and the generator
# (gen.lua) live alongside it; `make fetch-unicode` refreshes the
# data and regenerates the header.

UNICODE_DIR := $(VENDDIR)/unicode
