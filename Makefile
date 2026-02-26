# Hull — Makefile
#
# Builds Hull with QuickJS and Lua 5.4 runtimes.
# Vendors: QuickJS, Lua, Keel (linked as library).
#
# Usage:
#   make              # build hull binary (both runtimes)
#   make RUNTIME=js   # build with QuickJS runtime only
#   make RUNTIME=lua  # build with Lua runtime only
#   make test         # build and run tests
#   make debug        # debug build with ASan + UBSan
#   make msan         # MSan + UBSan (requires clang, Linux only)
#   make e2e          # end-to-end tests (JS + Lua runtimes)
#   make CC=cosmocc   # build with Cosmopolitan C (APE)
#   make clean        # remove build artifacts
#
# SPDX-License-Identifier: AGPL-3.0-or-later

CC      ?= cc
AR      ?= ar

# Runtime selection: "all" (default), "js", or "lua"
RUNTIME ?= all

# Detect Cosmopolitan toolchain
ifneq ($(findstring cosmocc,$(CC)),)
  COSMO := 1
endif

CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
ifndef COSMO
  CFLAGS += -fstack-protector-strong
endif
LDFLAGS :=

# Build mode
ifdef DEBUG
CFLAGS += -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
else
CFLAGS += -O2
endif

.DEFAULT_GOAL := all

# ── Directories ──────────────────────────────────────────────────────

SRCDIR   := src
INCDIR   := include
TESTDIR  := tests
BUILDDIR := build
VENDDIR  := vendor

# ── QuickJS ──────────────────────────────────────────────────────────

QJS_DIR  := $(VENDDIR)/quickjs
QJS_SRCS := $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
            $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/libbf.c
QJS_OBJS := $(patsubst $(QJS_DIR)/%.c,$(BUILDDIR)/qjs_%.o,$(QJS_SRCS))

# QuickJS compiled with relaxed warnings (vendored code)
QJS_CFLAGS := -std=c11 -O2 -w -DCONFIG_VERSION=\"2024-01-13\" \
              -DCONFIG_BIGNUM -D_GNU_SOURCE

# ── Lua 5.4 ──────────────────────────────────────────────────────────

LUA_DIR  := $(VENDDIR)/lua
LUA_SRCS := $(filter-out $(LUA_DIR)/lua.c $(LUA_DIR)/luac.c, \
             $(wildcard $(LUA_DIR)/*.c))
LUA_OBJS := $(patsubst $(LUA_DIR)/%.c,$(BUILDDIR)/lua_%.o,$(LUA_SRCS))

# Lua compiled with relaxed warnings (vendored code)
LUA_CFLAGS := -std=c11 -O2 -w -DLUA_USE_POSIX

# ── Keel (external library) ─────────────────────────────────────────

# Keel is included as a git submodule in vendor/keel.
# Override KEEL_DIR to point to a different Keel build if needed.
KEEL_DIR   ?= $(VENDDIR)/keel
KEEL_INC   := $(KEEL_DIR)/include
KEEL_LIB   := $(KEEL_DIR)/libkeel.a

# Build Keel if not already built
$(KEEL_LIB):
	$(MAKE) -C $(KEEL_DIR) CC=$(CC) AR=$(AR)

# ── SQLite (vendored amalgamation) ─────────────────────────────────

SQLITE_DIR    := $(VENDDIR)/sqlite
SQLITE_OBJ    := $(BUILDDIR)/sqlite3.o
SQLITE_CFLAGS := -std=c11 -O2 -w -DSQLITE_THREADSAFE=1

# ── Hull source files ───────────────────────────────────────────────

# Capability sources (always compiled)
CAP_SRCS := $(wildcard $(SRCDIR)/cap/*.c)
CAP_OBJS := $(patsubst $(SRCDIR)/cap/%.c,$(BUILDDIR)/cap_%.o,$(CAP_SRCS))

# JS runtime sources
JS_RT_SRCS := $(wildcard $(SRCDIR)/runtime/js/*.c)
JS_RT_OBJS := $(patsubst $(SRCDIR)/runtime/js/%.c,$(BUILDDIR)/js_%.o,$(JS_RT_SRCS))

# Lua runtime sources
LUA_RT_SRCS := $(wildcard $(SRCDIR)/runtime/lua/*.c)
LUA_RT_OBJS := $(patsubst $(SRCDIR)/runtime/lua/%.c,$(BUILDDIR)/lua_rt_%.o,$(LUA_RT_SRCS))

# Select which runtimes to build
ifeq ($(RUNTIME),js)
  RT_OBJS   := $(JS_RT_OBJS)
  VEND_OBJS := $(QJS_OBJS)
  CFLAGS    += -DHL_ENABLE_JS
else ifeq ($(RUNTIME),lua)
  RT_OBJS   := $(LUA_RT_OBJS)
  VEND_OBJS := $(LUA_OBJS)
  CFLAGS    += -DHL_ENABLE_LUA
else
  # default: both runtimes
  RT_OBJS   := $(JS_RT_OBJS) $(LUA_RT_OBJS)
  VEND_OBJS := $(QJS_OBJS) $(LUA_OBJS)
  CFLAGS    += -DHL_ENABLE_JS -DHL_ENABLE_LUA
endif

MAIN_OBJ := $(BUILDDIR)/main.o

# ── Include paths ───────────────────────────────────────────────────

INCLUDES := -I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(KEEL_INC) -I$(SQLITE_DIR)

# ── Targets ─────────────────────────────────────────────────────────

.PHONY: all clean test debug msan e2e check analyze cppcheck bench

all: $(BUILDDIR)/hull

# Hull binary
$(BUILDDIR)/hull: $(CAP_OBJS) $(RT_OBJS) $(MAIN_OBJ) $(VEND_OBJS) $(SQLITE_OBJ) $(KEEL_LIB)
	$(CC) $(LDFLAGS) -o $@ $(CAP_OBJS) $(RT_OBJS) $(MAIN_OBJ) $(VEND_OBJS) \
		$(SQLITE_OBJ) $(KEEL_LIB) -lm -lpthread

# Capability sources
$(BUILDDIR)/cap_%.o: $(SRCDIR)/cap/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# JS runtime sources
$(BUILDDIR)/js_%.o: $(SRCDIR)/runtime/js/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Lua runtime sources
$(BUILDDIR)/lua_rt_%.o: $(SRCDIR)/runtime/lua/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Main
$(BUILDDIR)/main.o: $(SRCDIR)/main.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# QuickJS sources (relaxed warnings)
$(BUILDDIR)/qjs_%.o: $(QJS_DIR)/%.c | $(BUILDDIR)
	$(CC) $(QJS_CFLAGS) -I$(QJS_DIR) -c -o $@ $<

# Lua sources (relaxed warnings)
$(BUILDDIR)/lua_%.o: $(LUA_DIR)/%.c | $(BUILDDIR)
	$(CC) $(LUA_CFLAGS) -I$(LUA_DIR) -c -o $@ $<

# SQLite amalgamation (vendored, relaxed warnings)
$(SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c | $(BUILDDIR)
	$(CC) $(SQLITE_CFLAGS) -I$(SQLITE_DIR) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ── Debug build ─────────────────────────────────────────────────────

debug:
	$(MAKE) clean
	$(MAKE) DEBUG=1 all

# ── Tests ───────────────────────────────────────────────────────────

TEST_SRCS := $(wildcard $(TESTDIR)/test_*.c)

# Filter test binaries based on RUNTIME selection
ifeq ($(RUNTIME),js)
  TEST_SRCS := $(filter-out $(TESTDIR)/test_lua_runtime.c,$(TEST_SRCS))
else ifeq ($(RUNTIME),lua)
  TEST_SRCS := $(filter-out $(TESTDIR)/test_js_runtime.c,$(TEST_SRCS))
endif

TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(TEST_SRCS))

# Test objects need hull capability sources but NOT main.o or runtime objects
TEST_CAP_OBJS := $(CAP_OBJS)

# JS runtime objects needed for test_js_runtime
TEST_JS_OBJS := $(JS_RT_OBJS)

# Lua runtime objects needed for test_lua_runtime
TEST_LUA_OBJS := $(LUA_RT_OBJS)

# Default test rule (capability tests — Keel needed for body reader cap)
$(BUILDDIR)/test_%: $(TESTDIR)/test_%.c $(TEST_CAP_OBJS) $(SQLITE_OBJ) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_CAP_OBJS) \
		$(KEEL_LIB) $(SQLITE_OBJ) -lm -lpthread

# JS runtime test — needs QuickJS + JS runtime objects + Keel
$(BUILDDIR)/test_js_runtime: $(TESTDIR)/test_js_runtime.c $(TEST_CAP_OBJS) $(TEST_JS_OBJS) $(QJS_OBJS) $(SQLITE_OBJ) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(TEST_JS_OBJS) $(QJS_OBJS) \
		$(KEEL_LIB) $(SQLITE_OBJ) -lm -lpthread

# Lua runtime test — needs Lua + Lua runtime objects + Keel
$(BUILDDIR)/test_lua_runtime: $(TESTDIR)/test_lua_runtime.c $(TEST_CAP_OBJS) $(TEST_LUA_OBJS) $(LUA_OBJS) $(SQLITE_OBJ) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(TEST_LUA_OBJS) $(LUA_OBJS) \
		$(KEEL_LIB) $(SQLITE_OBJ) -lm -lpthread

test: $(TEST_BINS)
	@echo "Running tests..."
	@pass=0; fail=0; total=0; \
	for t in $(TEST_BINS); do \
		total=$$((total + 1)); \
		echo "=== $$(basename $$t) ==="; \
		if $$t; then \
			pass=$$((pass + 1)); \
		else \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "$$pass/$$total tests passed"; \
	if [ $$fail -gt 0 ]; then exit 1; fi

# ── MSan build (requires clang, Linux only) ────────────────────────

MSAN_CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
                -g -O1 -fsanitize=memory,undefined -fno-omit-frame-pointer
MSAN_LDFLAGS := -fsanitize=memory,undefined

msan:
	$(MAKE) clean
	$(MAKE) CC=clang \
		CFLAGS="$(MSAN_CFLAGS)" \
		LDFLAGS="$(MSAN_LDFLAGS)" \
		QJS_CFLAGS="-std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer -DCONFIG_VERSION=\"2024-01-13\" -DCONFIG_BIGNUM -D_GNU_SOURCE" \
		LUA_CFLAGS="-std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer -DLUA_USE_POSIX" \
		SQLITE_CFLAGS="-std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer -DSQLITE_THREADSAFE=1" \
		test

# ── E2E tests ──────────────────────────────────────────────────────

e2e: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e.sh

# ── Full check (sanitized build + test + e2e) ───────────────────────

check:
	$(MAKE) clean
	$(MAKE) DEBUG=1 all test e2e

# ── Static analysis ─────────────────────────────────────────────────

analyze:
	scan-build --status-bugs $(MAKE) clean all

cppcheck:
	cppcheck --enable=all --inline-suppr \
		--suppress=missingIncludeSystem \
		--suppress=unusedFunction \
		--suppress=checkersReport \
		--suppress=toomanyconfigs \
		--suppress=normalCheckLevelMaxBranches \
		--suppress=constParameterCallback \
		--suppress=constParameterPointer \
		--suppress=constVariablePointer \
		--suppress=staticFunction \
		--suppress='*:$(QJS_DIR)/*' \
		--suppress='*:$(LUA_DIR)/*' \
		--suppress='*:$(SQLITE_DIR)/*' \
		--error-exitcode=1 \
		-I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(SQLITE_DIR) -I$(KEEL_INC) \
		$(SRCDIR)/main.c $(SRCDIR)/cap/*.c $(SRCDIR)/runtime/js/*.c \
		$(SRCDIR)/runtime/lua/*.c

# ── Benchmark ──────────────────────────────────────────────────────

bench: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh bench.sh

# ── Clean ───────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR)
