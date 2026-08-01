# mk/vendor/tcc.mk - TinyCC embedded C compiler config (ELF/Linux-only,
# HL_ENABLE_TCC). Feature-local platform conditional stays inline here.

# ── TinyCC (embedded C compiler for zero-dependency hull build) ──────
#
# On by default for non-cosmo builds. Disable with HL_ENABLE_TCC=0.
# Requires vendor/tcc submodule (git submodule add -b mob ...)
#
# Build tcc: make tcc  (builds build/tcc from vendor/tcc source)

TCC_DIR         := vendor/tcc
COMPILER_OBJ    := $(BUILDDIR)/compiler.o
COMPILER_TCC_OBJ :=

# tcc emits ELF, so the backend is only useful on Linux. macOS (Mach-O) and
# cosmo (APE archives) use the system compiler, so tcc is off there. On Linux
# the backend is compiled in but tcc itself is NO LONGER EMBEDDED — it's a
# side-loaded tool (`hull tools install tcc`), resolved at build time from
# ~/.hull/tools → PATH. See compiler_tcc.c.
ifdef COSMO
  HL_ENABLE_TCC ?= 0
else ifeq ($(UNAME_S),Darwin)
  HL_ENABLE_TCC ?= 0
else
  HL_ENABLE_TCC ?= 1
endif

