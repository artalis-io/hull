# Hull — Makefile
#
# Builds Hull with QuickJS and Lua 5.4 runtimes.
# Vendors: QuickJS, Lua, Keel (linked as library).
#
# Usage:
#   make              # build hull binary
#   make test         # build and run tests
#   make debug        # debug build with ASan + UBSan
#   make clean        # remove build artifacts
#
# SPDX-License-Identifier: AGPL-3.0-or-later

CC      ?= cc
AR      ?= ar
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
CFLAGS  += -fstack-protector-strong
LDFLAGS :=

# Build mode
ifdef DEBUG
CFLAGS += -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
else
CFLAGS += -O2
endif

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

# Keel is expected to be built separately and available as a library.
# Set KEEL_DIR to point to the Keel source/build directory.
KEEL_DIR   ?= ../keel
KEEL_INC   := $(KEEL_DIR)/include
KEEL_LIB   := $(KEEL_DIR)/libkeel.a

# ── SQLite (vendored or system) ─────────────────────────────────────

# For now, link against system SQLite. TODO: vendor SQLite source.
SQLITE_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
SQLITE_LIBS   := $(shell pkg-config --libs sqlite3 2>/dev/null || echo "-lsqlite3")

# ── Hull source files ───────────────────────────────────────────────

HULL_SRCS := $(SRCDIR)/hull_cap_db.c \
             $(SRCDIR)/hull_cap_fs.c \
             $(SRCDIR)/hull_cap_time.c \
             $(SRCDIR)/hull_cap_env.c \
             $(SRCDIR)/hull_cap_crypto.c \
             $(SRCDIR)/js_runtime.c \
             $(SRCDIR)/js_bindings.c \
             $(SRCDIR)/js_modules.c \
             $(SRCDIR)/lua_runtime.c \
             $(SRCDIR)/lua_bindings.c \
             $(SRCDIR)/lua_modules.c

HULL_OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(HULL_SRCS))
MAIN_OBJ  := $(BUILDDIR)/main.o

# ── Include paths ───────────────────────────────────────────────────

INCLUDES := -I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(KEEL_INC) $(SQLITE_CFLAGS)

# ── Targets ─────────────────────────────────────────────────────────

.PHONY: all clean test debug analyze cppcheck

all: $(BUILDDIR)/hull

# Hull binary
$(BUILDDIR)/hull: $(HULL_OBJS) $(MAIN_OBJ) $(QJS_OBJS) $(LUA_OBJS) $(KEEL_LIB)
	$(CC) $(LDFLAGS) -o $@ $(HULL_OBJS) $(MAIN_OBJ) $(QJS_OBJS) $(LUA_OBJS) \
		$(KEEL_LIB) $(SQLITE_LIBS) -lm -lpthread

# Hull C sources
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
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

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ── Debug build ─────────────────────────────────────────────────────

debug:
	$(MAKE) clean
	$(MAKE) DEBUG=1 all

# ── Tests ───────────────────────────────────────────────────────────

TEST_SRCS := $(wildcard $(TESTDIR)/test_*.c)
TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(TEST_SRCS))

# Test objects need hull capability sources but NOT main.o or js_runtime
TEST_CAP_OBJS := $(BUILDDIR)/hull_cap_db.o \
                 $(BUILDDIR)/hull_cap_fs.o \
                 $(BUILDDIR)/hull_cap_time.o \
                 $(BUILDDIR)/hull_cap_env.o \
                 $(BUILDDIR)/hull_cap_crypto.o

# Lua runtime objects needed for test_lua_runtime
TEST_LUA_OBJS := $(BUILDDIR)/lua_runtime.o \
                 $(BUILDDIR)/lua_modules.o \
                 $(BUILDDIR)/lua_bindings.o

# JS runtime objects needed for test_js_runtime
TEST_JS_OBJS := $(BUILDDIR)/js_runtime.o \
                $(BUILDDIR)/js_modules.o \
                $(BUILDDIR)/js_bindings.o

# Default test rule (capability tests — no runtime objects needed)
$(BUILDDIR)/test_%: $(TESTDIR)/test_%.c $(TEST_CAP_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_CAP_OBJS) \
		$(SQLITE_LIBS) -lm

# JS runtime test — needs QuickJS + JS runtime objects + Keel
$(BUILDDIR)/test_js_runtime: $(TESTDIR)/test_js_runtime.c $(TEST_CAP_OBJS) $(TEST_JS_OBJS) $(QJS_OBJS) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(TEST_JS_OBJS) $(QJS_OBJS) \
		$(KEEL_LIB) $(SQLITE_LIBS) -lm -lpthread

# Lua runtime test — needs Lua + Lua runtime objects + Keel
$(BUILDDIR)/test_lua_runtime: $(TESTDIR)/test_lua_runtime.c $(TEST_CAP_OBJS) $(TEST_LUA_OBJS) $(LUA_OBJS) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(TEST_LUA_OBJS) $(LUA_OBJS) \
		$(KEEL_LIB) $(SQLITE_LIBS) -lm -lpthread

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

# ── Static analysis ─────────────────────────────────────────────────

analyze:
	scan-build --status-bugs $(MAKE) clean all

cppcheck:
	cppcheck --enable=all --error-exitcode=1 \
		-I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(KEEL_INC) \
		$(SRCDIR)/*.c

# ── Clean ───────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR)
