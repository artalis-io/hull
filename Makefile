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
  AR    := cosmoar
endif

# Platform detection
UNAME_S := $(shell uname -s)

CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
ifndef COSMO
  CFLAGS += -fstack-protector-strong
  ifeq ($(UNAME_S),Linux)
    CFLAGS += -D_DEFAULT_SOURCE
  endif
endif
LDFLAGS :=

# Build mode
ifdef DEBUG
CFLAGS += -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
else
CFLAGS += -O2
endif

ifdef COVERAGE
CFLAGS  += -g -O0 --coverage
LDFLAGS += --coverage
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

# ── rxi/log.c ─────────────────────────────────────────────────────────

LOG_DIR    := $(VENDDIR)/log.c
LOG_OBJ    := $(BUILDDIR)/log.o
LOG_CFLAGS := -std=c11 -O2 -w -DLOG_USE_COLOR

# ── sh_arena (vendored from otto) ────────────────────────────────────

SH_ARENA_DIR    := $(VENDDIR)/sh_arena
SH_ARENA_OBJ    := $(BUILDDIR)/sh_arena.o
SH_ARENA_CFLAGS := -std=c11 -O2 -w

# ── TweetNaCl (Ed25519 signatures) ─────────────────────────────────

TWEETNACL_DIR    := $(VENDDIR)/tweetnacl
TWEETNACL_OBJ    := $(BUILDDIR)/tweetnacl.o
TWEETNACL_CFLAGS := -std=c11 -O2 -w

# ── jart/pledge polyfill (Linux-only: seccomp + landlock) ──────────
#
# Provides real pledge()/unveil() on native Linux.
# Cosmopolitan has these built-in; macOS uses no-op stubs.

PLEDGE_DIR := $(VENDDIR)/pledge
PLEDGE_CFLAGS := -std=c11 -O2 -w -D_GNU_SOURCE -I$(PLEDGE_DIR)

ifeq ($(UNAME_S),Linux)
ifndef COSMO
PLEDGE_SRCS := \
	$(PLEDGE_DIR)/libc/calls/pledge.c \
	$(PLEDGE_DIR)/libc/calls/pledge-linux.c \
	$(PLEDGE_DIR)/libc/calls/unveil.c \
	$(PLEDGE_DIR)/libc/calls/parsepromises.c \
	$(PLEDGE_DIR)/libc/calls/landlock_add_rule.c \
	$(PLEDGE_DIR)/libc/calls/landlock_create_ruleset.c \
	$(PLEDGE_DIR)/libc/calls/landlock_restrict_self.c \
	$(PLEDGE_DIR)/libc/calls/commandv.c \
	$(PLEDGE_DIR)/libc/calls/getcpucount.c \
	$(PLEDGE_DIR)/libc/calls/islinux.c \
	$(PLEDGE_DIR)/libc/intrin/promises.c \
	$(PLEDGE_DIR)/libc/intrin/pthread_setcancelstate.c \
	$(PLEDGE_DIR)/libc/elf/checkelfaddress.c \
	$(PLEDGE_DIR)/libc/elf/getelfsegmentheaderaddress.c \
	$(PLEDGE_DIR)/libc/str/classifypath.c \
	$(PLEDGE_DIR)/libc/str/endswith.c \
	$(PLEDGE_DIR)/libc/str/isabspath.c \
	$(PLEDGE_DIR)/libc/fmt/joinpaths.c \
	$(PLEDGE_DIR)/libc/fmt/sizetol.c \
	$(PLEDGE_DIR)/libc/runtime/isdynamicexecutable.c \
	$(PLEDGE_DIR)/libc/sysv/calls/ioprio_set.c \
	$(PLEDGE_DIR)/libc/x/xdie.c \
	$(PLEDGE_DIR)/libc/x/xjoinpaths.c \
	$(PLEDGE_DIR)/libc/x/xmalloc.c \
	$(PLEDGE_DIR)/libc/x/xrealloc.c \
	$(PLEDGE_DIR)/libc/x/xstrcat.c \
	$(PLEDGE_DIR)/libc/x/xstrdup.c
PLEDGE_OBJS := $(patsubst $(PLEDGE_DIR)/%.c,$(BUILDDIR)/pledge_%.o,$(PLEDGE_SRCS))
endif
endif
PLEDGE_OBJS ?=

# ── Hull source files ───────────────────────────────────────────────

# Capability sources (always compiled, except cap/tool.c and cap/test.c which need runtimes)
CAP_SRCS := $(filter-out $(SRCDIR)/hull/cap/tool.c $(SRCDIR)/hull/cap/test.c,$(wildcard $(SRCDIR)/hull/cap/*.c))
CAP_OBJS := $(patsubst $(SRCDIR)/hull/cap/%.c,$(BUILDDIR)/cap_%.o,$(CAP_SRCS))
CAP_TOOL_OBJ := $(BUILDDIR)/cap_tool.o
CAP_TEST_OBJ := $(BUILDDIR)/cap_test.o

# JS runtime sources
JS_RT_SRCS := $(wildcard $(SRCDIR)/hull/runtime/js/*.c)
JS_RT_OBJS := $(patsubst $(SRCDIR)/hull/runtime/js/%.c,$(BUILDDIR)/js_%.o,$(JS_RT_SRCS))

# Lua runtime sources
LUA_RT_SRCS := $(wildcard $(SRCDIR)/hull/runtime/lua/*.c)
LUA_RT_OBJS := $(patsubst $(SRCDIR)/hull/runtime/lua/%.c,$(BUILDDIR)/lua_rt_%.o,$(LUA_RT_SRCS))

# Command module sources
CMD_SRCS := $(wildcard $(SRCDIR)/hull/commands/*.c)
CMD_OBJS := $(patsubst $(SRCDIR)/hull/commands/%.c,$(BUILDDIR)/cmd_%.o,$(CMD_SRCS))

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

ALLOC_OBJ      := $(BUILDDIR)/hull_alloc.o
MANIFEST_OBJ   := $(BUILDDIR)/manifest.o
SANDBOX_OBJ    := $(BUILDDIR)/sandbox.o

# Test-specific manifest objects (single runtime — avoids pulling Lua into JS tests and vice versa)
MANIFEST_JS_OBJ  := $(BUILDDIR)/manifest_js_only.o
MANIFEST_LUA_OBJ := $(BUILDDIR)/manifest_lua_only.o
TOOL_OBJ       := $(BUILDDIR)/tool.o
BUILD_ASSET_OBJ := $(BUILDDIR)/build_assets.o
MAIN_OBJ       := $(BUILDDIR)/main.o
ENTRY_OBJ      := $(BUILDDIR)/entry.o

# ── Stdlib embedding (xxd) ──────────────────────────────────────────
#
# All .lua files under stdlib/lua/ (excluding tests/) are converted to
# C byte arrays at build time via xxd -i. Path separators are flattened
# to underscores: stdlib/lua/vendor/json.lua → build/stdlib_lua_vendor_json.h

STDLIB_LUA_FILES := $(shell find stdlib/lua -name '*.lua' -not -path '*/tests/*' 2>/dev/null)

# Flatten path: stdlib/lua/vendor/json.lua → build/stdlib_lua_vendor_json.h
stdlib_hdr = $(BUILDDIR)/$(subst /,_,$(patsubst stdlib/%.lua,stdlib_%.h,$(1)))
STDLIB_LUA_HDRS := $(foreach f,$(STDLIB_LUA_FILES),$(call stdlib_hdr,$(f)))

# Auto-generated registry: includes all xxd headers and builds a module table
STDLIB_LUA_REGISTRY := $(BUILDDIR)/stdlib_lua_registry.h

# Generate per-file xxd rules (avoids % matching directory separators)
define STDLIB_LUA_RULE
$(call stdlib_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(STDLIB_LUA_FILES),$(eval $(call STDLIB_LUA_RULE,$(f))))

# Keep separate list of xxd-only headers (without the registry itself)
STDLIB_LUA_XXD_HDRS := $(STDLIB_LUA_HDRS)

# Generate the auto-registry header from discovered files
$(STDLIB_LUA_REGISTRY): $(STDLIB_LUA_XXD_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated — do not edit */" > $@
	@for hdr in $(STDLIB_LUA_XXD_HDRS); do \
		echo "#include \"$$(basename $$hdr)\""; \
	done >> $@
	@echo "" >> $@
	@echo "typedef struct { const char *name; const unsigned char *data; unsigned int len; } HlStdlibEntry;" >> $@
	@echo "static const HlStdlibEntry hl_stdlib_lua_entries[] = {" >> $@
	@for f in $(STDLIB_LUA_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/lua/||; s|\.lua$$||; s|/|.|g'); \
		echo "    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done >> $@
	@echo "    { 0, 0, 0 }" >> $@
	@echo "};" >> $@

STDLIB_LUA_HDRS += $(STDLIB_LUA_REGISTRY)

# ── App code embedding (xxd) ─────────────────────────────────────────
#
# When APP_DIR is set (e.g. make APP_DIR=myapp), all .lua files under
# APP_DIR are embedded into the binary using the same xxd pattern as stdlib.
# Module names use relative paths from APP_DIR with ./ prefix:
#   myapp/routes/users.lua → "./routes/users"

APP_ENTRIES_DEFAULT_OBJ := $(BUILDDIR)/app_entries_default.o
APP_DIR ?=
APP_EXTRA_OBJS := $(APP_ENTRIES_DEFAULT_OBJ)
ifneq ($(APP_DIR),)
APP_LUA_FILES := $(shell find $(APP_DIR) -name '*.lua' -not -path '*/tests/*' 2>/dev/null)

# Flatten path: myapp/routes/users.lua → build/app_lua_routes_users.h
app_hdr = $(BUILDDIR)/app_lua_$(subst /,_,$(patsubst $(APP_DIR)/%.lua,%.h,$(1)))
APP_LUA_HDRS := $(foreach f,$(APP_LUA_FILES),$(call app_hdr,$(f)))

APP_LUA_REGISTRY := $(BUILDDIR)/app_lua_registry.h

define APP_LUA_RULE
$(call app_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(APP_LUA_FILES),$(eval $(call APP_LUA_RULE,$(f))))

APP_LUA_XXD_HDRS := $(APP_LUA_HDRS)

APP_LUA_REGISTRY_C := $(BUILDDIR)/app_lua_registry.c
APP_LUA_REGISTRY_O := $(BUILDDIR)/app_lua_registry.o

$(APP_LUA_REGISTRY_C): $(APP_LUA_XXD_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated — do not edit */" > $@
	@for hdr in $(APP_LUA_XXD_HDRS); do \
		echo "#include \"$$(basename $$hdr)\""; \
	done >> $@
	@echo "" >> $@
	@echo "typedef struct { const char *name; const unsigned char *data; unsigned int len; } HlStdlibEntry;" >> $@
	@echo "const HlStdlibEntry hl_app_lua_entries[] = {" >> $@
	@for f in $(APP_LUA_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^$(APP_DIR)/||; s|\.lua$$||'); \
		echo "    { \"./$$modname\", $${varname}, sizeof($${varname}) },"; \
	done >> $@
	@echo "    { 0, 0, 0 }" >> $@
	@echo "};" >> $@

$(APP_LUA_REGISTRY_O): $(APP_LUA_REGISTRY_C) | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -I$(BUILDDIR) -c -o $@ $<

STDLIB_LUA_HDRS += $(APP_LUA_HDRS)
APP_EXTRA_OBJS := $(APP_LUA_REGISTRY_O)
endif

# App entries default (empty array — used when no APP_DIR)
$(APP_ENTRIES_DEFAULT_OBJ): $(SRCDIR)/hull/app_entries_default.c | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -c -o $@ $<

# ── Include paths ───────────────────────────────────────────────────

INCLUDES := -I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(KEEL_INC) -I$(SQLITE_DIR) -I$(LOG_DIR) -I$(SH_ARENA_DIR) -I$(TWEETNACL_DIR) -I$(BUILDDIR)

# ── Targets ─────────────────────────────────────────────────────────

.PHONY: all clean test debug msan e2e e2e-build e2e-sandbox self-build check analyze cppcheck bench coverage lint-lua lint-js lint platform

all: $(BUILDDIR)/hull

# Platform static library — everything except entry.o and build_assets.o
# Used by `hull build` to produce standalone app binaries.
# Exports hull_main() (subcommand dispatch + server logic).
PLATFORM_OBJS := $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(MANIFEST_OBJ) $(SANDBOX_OBJ) $(MAIN_OBJ) $(TOOL_OBJ) $(VEND_OBJS) \
	$(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(TWEETNACL_OBJ) $(PLEDGE_OBJS)

PLATFORM_LIB := $(BUILDDIR)/libhull_platform.a

$(PLATFORM_LIB): $(PLATFORM_OBJS) $(KEEL_LIB) | $(BUILDDIR)
	@rm -f $@
	$(AR) rcs $@ $(PLATFORM_OBJS)
	@# Merge keel objects into the platform archive
	@tmpdir=$$(mktemp -d) && \
		cd $$tmpdir && \
		$(AR) x $(CURDIR)/$(KEEL_LIB) && \
		$(AR) rcs $(CURDIR)/$@ *.o && \
		rm -rf $$tmpdir
	@# Record the CC used so hull build can auto-detect
	@echo "$(CC)" > $(BUILDDIR)/platform_cc

platform: $(PLATFORM_LIB)

# ── Embedded build assets (distribution builds only) ────────────────
# Build with: make EMBED_PLATFORM=1
# This xxd's the platform .a + templates into build_assets.c
EMBED_PLATFORM ?=
ifneq ($(EMBED_PLATFORM),)
EMBEDDED_PLATFORM_H := $(BUILDDIR)/embedded_platform.h
EMBEDDED_TEMPLATES_H := $(BUILDDIR)/embedded_templates.h

$(EMBEDDED_PLATFORM_H): $(PLATFORM_LIB) | $(BUILDDIR)
	xxd -i $< | sed 's/build_libhull_platform_a/hl_embedded_platform_a/g' > $@

$(EMBEDDED_TEMPLATES_H): templates/app_main.c templates/entry.h | $(BUILDDIR)
	@echo "/* Auto-generated — do not edit */" > $@
	@xxd -i templates/app_main.c | sed 's/templates_app_main_c/hl_embedded_app_main_c/g' >> $@
	@xxd -i templates/entry.h | sed 's/templates_entry_h/hl_embedded_entry_h/g' >> $@

CFLAGS += -DHL_BUILD_EMBEDDED
$(BUILD_ASSET_OBJ): $(EMBEDDED_PLATFORM_H) $(EMBEDDED_TEMPLATES_H)
endif

# Hull binary
$(BUILDDIR)/hull: $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(MANIFEST_OBJ) $(SANDBOX_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_OBJ) $(MAIN_OBJ) $(ENTRY_OBJ) $(APP_EXTRA_OBJS) $(VEND_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(TWEETNACL_OBJ) $(PLEDGE_OBJS) $(KEEL_LIB)
	$(CC) $(LDFLAGS) -o $@ $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(MANIFEST_OBJ) $(SANDBOX_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_OBJ) $(MAIN_OBJ) $(ENTRY_OBJ) $(APP_EXTRA_OBJS) $(VEND_OBJS) \
		$(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(TWEETNACL_OBJ) $(PLEDGE_OBJS) $(KEEL_LIB) -lm -lpthread

# Capability sources
$(BUILDDIR)/cap_%.o: $(SRCDIR)/hull/cap/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Command module sources
$(BUILDDIR)/cmd_%.o: $(SRCDIR)/hull/commands/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# JS runtime sources
$(BUILDDIR)/js_%.o: $(SRCDIR)/hull/runtime/js/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Lua runtime sources (depend on generated stdlib headers)
$(BUILDDIR)/lua_rt_%.o: $(SRCDIR)/hull/runtime/lua/%.c $(STDLIB_LUA_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Hull allocator
$(ALLOC_OBJ): $(SRCDIR)/hull/alloc.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Manifest
$(MANIFEST_OBJ): $(SRCDIR)/hull/manifest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Manifest (JS-only, for test_js — excludes Lua extraction to avoid Lua link deps)
$(MANIFEST_JS_OBJ): $(SRCDIR)/hull/manifest.c | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA,$(CFLAGS)) $(INCLUDES) -c -o $@ $<

# Manifest (Lua-only, for test_lua — excludes JS extraction to avoid QuickJS link deps)
$(MANIFEST_LUA_OBJ): $(SRCDIR)/hull/manifest.c | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -c -o $@ $<

# Sandbox (pledge/unveil enforcement)
$(SANDBOX_OBJ): $(SRCDIR)/hull/sandbox.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Tool mode (keygen, build, verify, etc.)
$(TOOL_OBJ): $(SRCDIR)/hull/tool.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Build assets (embedded platform lib — stub unless HL_BUILD_EMBEDDED=1)
$(BUILD_ASSET_OBJ): $(SRCDIR)/hull/build_assets.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Main (hull_main — goes into platform .a)
$(BUILDDIR)/main.o: $(SRCDIR)/hull/main.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Entry (thin main → hull_main trampoline — NOT in platform .a)
$(ENTRY_OBJ): $(SRCDIR)/hull/entry.c | $(BUILDDIR)
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

# rxi/log.c (vendored, relaxed warnings)
$(LOG_OBJ): $(LOG_DIR)/log.c | $(BUILDDIR)
	$(CC) $(LOG_CFLAGS) -I$(LOG_DIR) -c -o $@ $<

# sh_arena (vendored, relaxed warnings)
$(SH_ARENA_OBJ): $(SH_ARENA_DIR)/sh_arena.c | $(BUILDDIR)
	$(CC) $(SH_ARENA_CFLAGS) -I$(SH_ARENA_DIR) -c -o $@ $<

# TweetNaCl (vendored, relaxed warnings)
$(TWEETNACL_OBJ): $(TWEETNACL_DIR)/tweetnacl.c | $(BUILDDIR)
	$(CC) $(TWEETNACL_CFLAGS) -I$(TWEETNACL_DIR) -c -o $@ $<

# jart/pledge polyfill (vendored, Linux only, relaxed warnings)
# Flatten libc/calls/pledge.c → build/pledge_libc_calls_pledge.o
$(BUILDDIR)/pledge_%.o: $(PLEDGE_DIR)/%.c | $(BUILDDIR)
	$(CC) $(PLEDGE_CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ── Debug build ─────────────────────────────────────────────────────

debug:
	$(MAKE) clean
	$(MAKE) DEBUG=1 all

# ── Tests ───────────────────────────────────────────────────────────

# Discover all test sources under tests/hull/
TEST_SRCS := $(shell find $(TESTDIR)/hull -name 'test_*.c')

# Filter test binaries based on RUNTIME selection
ifeq ($(RUNTIME),js)
  TEST_SRCS := $(filter-out %/test_lua.c,$(TEST_SRCS))
else ifeq ($(RUNTIME),lua)
  TEST_SRCS := $(filter-out %/test_js.c,$(TEST_SRCS))
endif

# Flatten test paths to build/ binaries: tests/hull/cap/test_body.c → build/test_body
TEST_BINS := $(addprefix $(BUILDDIR)/,$(notdir $(basename $(TEST_SRCS))))

# Test objects need hull capability sources but NOT main.o or runtime objects
TEST_CAP_OBJS := $(CAP_OBJS)

# Shared link deps for all tests
TEST_COMMON_DEPS := $(TEST_CAP_OBJS) $(ALLOC_OBJ) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(TWEETNACL_OBJ) $(KEEL_LIB)
TEST_COMMON_LIBS := $(TEST_CAP_OBJS) $(ALLOC_OBJ) $(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(TWEETNACL_OBJ) -lm -lpthread

# Capability tests (tests/hull/cap/)
$(BUILDDIR)/test_%: $(TESTDIR)/hull/cap/test_%.c $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_COMMON_LIBS)

# Top-level tests (tests/hull/)
$(BUILDDIR)/test_parse_size: $(TESTDIR)/hull/test_parse_size.c $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_COMMON_LIBS)

# JS runtime test — needs QuickJS + JS runtime objects + manifest (JS-only to avoid Lua link deps)
$(BUILDDIR)/test_js: $(TESTDIR)/hull/runtime/js/test_js.c $(TEST_COMMON_DEPS) $(MANIFEST_JS_OBJ) $(JS_RT_OBJS) $(QJS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(JS_RT_OBJS) $(MANIFEST_JS_OBJ) $(ALLOC_OBJ) $(QJS_OBJS) \
		$(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(TWEETNACL_OBJ) -lm -lpthread

# Lua runtime test — needs Lua + Lua runtime objects + stdlib headers + manifest (Lua-only) + cap_tool
$(BUILDDIR)/test_lua: $(TESTDIR)/hull/runtime/lua/test_lua.c $(TEST_COMMON_DEPS) $(CAP_TOOL_OBJ) $(MANIFEST_LUA_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(LUA_RT_OBJS) $(LUA_OBJS) $(STDLIB_LUA_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(CAP_TOOL_OBJ) $(LUA_RT_OBJS) $(MANIFEST_LUA_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(ALLOC_OBJ) $(LUA_OBJS) \
		$(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(TWEETNACL_OBJ) -lm -lpthread

# Tool hardening test — cap/tool.c compiled without runtime flags (self-contained C functions)
CAP_TOOL_NONE_OBJ := $(BUILDDIR)/cap_tool_none.o
$(CAP_TOOL_NONE_OBJ): $(SRCDIR)/hull/cap/tool.c | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/test_tool: $(TESTDIR)/hull/cap/test_tool.c $(CAP_TOOL_NONE_OBJ) | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(CAP_TOOL_NONE_OBJ)

# Command dispatcher test — needs full command set (symbol resolution for command table)
$(BUILDDIR)/test_dispatch: $(TESTDIR)/hull/commands/test_dispatch.c $(CMD_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(TOOL_OBJ) $(SANDBOX_OBJ) $(TEST_COMMON_DEPS) $(RT_OBJS) $(VEND_OBJS) $(MANIFEST_OBJ) $(BUILD_ASSET_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(PLEDGE_OBJS) $(STDLIB_LUA_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(CMD_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(TOOL_OBJ) $(SANDBOX_OBJ) \
		$(TEST_CAP_OBJS) $(RT_OBJS) $(MANIFEST_OBJ) $(BUILD_ASSET_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(ALLOC_OBJ) $(VEND_OBJS) \
		$(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(TWEETNACL_OBJ) $(PLEDGE_OBJS) -lm -lpthread

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
#
# Use MSAN=1 as an internal flag so that CFLAGS/QJS_CFLAGS etc. are set
# inside the Makefile (not on the command line), which avoids:
#  1. CFLAGS leaking into the Keel submodule build
#  2. Shell double-escaping mangling the CONFIG_VERSION string

ifdef MSAN
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
            -g -O1 -fsanitize=memory,undefined -fno-omit-frame-pointer \
            -D_DEFAULT_SOURCE
LDFLAGS  := -fsanitize=memory,undefined
QJS_CFLAGS := -std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer \
              -DCONFIG_VERSION=\"2024-01-13\" -DCONFIG_BIGNUM -D_GNU_SOURCE
LUA_CFLAGS := -std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer \
              -DLUA_USE_POSIX
SQLITE_CFLAGS := -std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer \
                 -DSQLITE_THREADSAFE=1
LOG_CFLAGS := -std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer \
              -DLOG_USE_COLOR
endif

msan:
	$(MAKE) clean
	$(MAKE) -C $(KEEL_DIR) clean
	$(MAKE) -C $(KEEL_DIR) CC=clang
	$(MAKE) CC=clang MSAN=1 test

# ── E2E tests ──────────────────────────────────────────────────────

e2e: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e.sh

e2e-build:
	sh tests/e2e_build.sh

e2e-sandbox: $(BUILDDIR)/hull
	sh tests/e2e_sandbox.sh

# ── Self-build (hull → hull2 → hull3 chain) ─────────────────────────

self-build: $(BUILDDIR)/hull platform
	@echo "=== Self-build: hull -> hull2 -> hull3 ==="
	@TMPDIR=$$(mktemp -d) && \
	$(BUILDDIR)/hull build -o "$$TMPDIR/hull2" tests/fixtures/null_app && \
	"$$TMPDIR/hull2" keygen "$$TMPDIR/key" && test -f "$$TMPDIR/key.pub" && \
	"$$TMPDIR/hull2" build -o "$$TMPDIR/hull3" tests/fixtures/null_app && \
	"$$TMPDIR/hull3" keygen "$$TMPDIR/key2" && test -f "$$TMPDIR/key2.pub" && \
	echo "PASS: self-build chain verified (hull -> hull2 -> hull3)" && \
	rm -rf "$$TMPDIR" || \
	(echo "FAIL: self-build chain" && rm -rf "$$TMPDIR" && exit 1)

# ── Full check (sanitized build + test + e2e) ───────────────────────

check:
	$(MAKE) clean
	$(MAKE) DEBUG=1 all test e2e

# ── Static analysis ─────────────────────────────────────────────────

analyze:
	$(MAKE) clean
	$(MAKE) $(VEND_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(KEEL_LIB)
	scan-build --status-bugs $(MAKE) $(CAP_OBJS) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(MAIN_OBJ) $(BUILDDIR)/hull

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
		--suppress=uninitvar:$(SRCDIR)/hull/runtime/lua/bindings.c \
		--suppress=unmatchedSuppression \
		--suppress='*:$(QJS_DIR)/*' \
		--suppress='*:$(LUA_DIR)/*' \
		--suppress='*:$(SQLITE_DIR)/*' \
		--suppress='*:$(LOG_DIR)/*' \
		--error-exitcode=1 \
		-I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(SQLITE_DIR) -I$(KEEL_INC) \
		$(SRCDIR)/hull/main.c $(SRCDIR)/hull/alloc.c $(SRCDIR)/hull/cap/*.c \
		$(SRCDIR)/hull/commands/*.c \
		$(SRCDIR)/hull/runtime/js/*.c $(SRCDIR)/hull/runtime/lua/*.c

# ── Benchmark ──────────────────────────────────────────────────────

bench: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh bench.sh

# ── Code coverage ────────────────────────────────────────────────────

coverage:
	$(MAKE) clean
	$(MAKE) COVERAGE=1 test
	mkdir -p $(BUILDDIR)/coverage
	lcov --capture --directory $(BUILDDIR) \
		--include '$(CURDIR)/src/*' \
		--output-file $(BUILDDIR)/coverage/coverage.info \
		--ignore-errors mismatch
	genhtml $(BUILDDIR)/coverage/coverage.info \
		--output-directory $(BUILDDIR)/coverage/html
	@echo "Coverage report: $(BUILDDIR)/coverage/html/index.html"

# ── Linting ──────────────────────────────────────────────────────────

lint-lua:
	luacheck stdlib/lua/hull/ examples/ --config .luacheckrc

lint-js:
	biome check examples/ --config-path biome.json

lint: lint-lua lint-js

# ── Clean ───────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR)
