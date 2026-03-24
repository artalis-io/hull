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

# Detect Cosmopolitan toolchain (cosmocc, x86_64-unknown-cosmo-cc, etc.)
ifneq ($(findstring cosmo,$(CC)),)
  COSMO := 1
endif
# Platform detection
UNAME_S := $(shell uname -s)

CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
ifndef COSMO
  CFLAGS += -fstack-protector-strong
  ifndef DEBUG
    CFLAGS += -D_FORTIFY_SOURCE=2
  endif
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

# Build Keel with mbedTLS backend
# Keel now detects the cosmo toolchain natively from CC and handles
# poll backend, .aarch64/ archive creation, etc.
MINIZ_DIR  := $(VENDDIR)/miniz

$(KEEL_LIB): $(MBEDTLS_OBJS)
	$(MAKE) -C $(KEEL_DIR) CC=$(CC) AR=$(AR) \
		KEEL_TLS=mbedtls MBEDTLS_CONFIG_FILE=hull_config.h \
		KEEL_COMPRESS=miniz MINIZ_DIR=$(CURDIR)/$(MINIZ_DIR)

# ── mbedTLS (vendored) ─────────────────────────────────────────────

MBEDTLS_DIR    := $(VENDDIR)/mbedtls
MBEDTLS_SRCS   := $(wildcard $(MBEDTLS_DIR)/library/*.c)
MBEDTLS_OBJS   := $(patsubst $(MBEDTLS_DIR)/library/%.c,$(BUILDDIR)/mbed_%.o,$(MBEDTLS_SRCS))
MBEDTLS_CFLAGS := -std=c11 -O2 -w \
	-I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library -I$(MBEDTLS_DIR) \
	-DMBEDTLS_CONFIG_FILE='"hull_config.h"'

$(BUILDDIR)/mbed_%.o: $(MBEDTLS_DIR)/library/%.c | $(BUILDDIR)
	$(CC) $(MBEDTLS_CFLAGS) -c -o $@ $<

# ── SQLite (vendored amalgamation) ─────────────────────────────────

SQLITE_DIR    := $(VENDDIR)/sqlite
SQLITE_OBJ    := $(BUILDDIR)/sqlite3.o
SQLITE_CFLAGS := -std=c11 -O2 -w -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5

# ── rxi/log.c ─────────────────────────────────────────────────────────

LOG_DIR    := $(VENDDIR)/log.c
LOG_OBJ    := $(BUILDDIR)/log.o
LOG_CFLAGS := -std=c11 -O2 -w -DLOG_USE_COLOR

# ── sh_arena (vendored from otto) ────────────────────────────────────

SH_ARENA_DIR    := $(VENDDIR)/sh_arena
SH_ARENA_OBJ    := $(BUILDDIR)/sh_arena.o
SH_ARENA_CFLAGS := -std=c11 -O2 -w

# ── sh_json (vendored from otto) ──────────────────────────────────────

SH_JSON_DIR    := $(VENDDIR)/sh_json
SH_JSON_OBJ    := $(BUILDDIR)/sh_json.o
SH_JSON_CFLAGS := -std=c11 -O2 -w

# ── TweetNaCl (Ed25519 signatures) ─────────────────────────────────

TWEETNACL_DIR    := $(VENDDIR)/tweetnacl
TWEETNACL_OBJ    := $(BUILDDIR)/tweetnacl.o
TWEETNACL_CFLAGS := -std=c11 -O2 -w

# ── WAMR (WebAssembly Micro Runtime — compute-only) ──────────────
#
# Optional 7th vendored C library. Provides near-native WASM execution
# for CPU-intensive compute plugins. No WASI, no I/O.
# Disable with: make HL_ENABLE_WASM=0

HL_ENABLE_WASM ?= 1

ifeq ($(HL_ENABLE_WASM),1)
CFLAGS += -DHL_ENABLE_WASM

# User-configurable WASM ceilings (override: make HL_WASM_MAX_HEAP_MB=512)
ifneq ($(HL_WASM_MAX_HEAP_MB),)
CFLAGS += '-DHL_WASM_MAX_HEAP=((uint32_t)$(HL_WASM_MAX_HEAP_MB)*1024*1024)'
endif
ifneq ($(HL_WASM_MAX_STACK_MB),)
CFLAGS += '-DHL_WASM_MAX_STACK=((uint32_t)$(HL_WASM_MAX_STACK_MB)*1024*1024)'
endif
ifneq ($(HL_WASM_MAX_IO_MB),)
CFLAGS += '-DHL_WASM_MAX_IO_SIZE=((uint32_t)$(HL_WASM_MAX_IO_MB)*1024*1024)'
endif

WAMR_DIR     := $(VENDDIR)/wamr
WAMR_CORE    := $(WAMR_DIR)/core
WAMR_IWASM   := $(WAMR_CORE)/iwasm
WAMR_SHARED  := $(WAMR_CORE)/shared

# Platform-specific arch reloc + platform_init
ifeq ($(UNAME_S),Darwin)
WAMR_PLATFORM_INIT := $(WAMR_SHARED)/platform/darwin/platform_init.c
WAMR_PLATFORM_HDR  := $(WAMR_SHARED)/platform/darwin
WAMR_ARCH_RELOC    := $(WAMR_IWASM)/aot/arch/aot_reloc_aarch64.c
WAMR_ARCH_DEFS     := -DBUILD_TARGET=\"AARCH64\" -DWASM_HAVE_MREMAP=0
WAMR_MREMAP_SRC    := $(WAMR_SHARED)/platform/common/memory/mremap.c
  ifeq ($(shell uname -m),x86_64)
    WAMR_ARCH_RELOC := $(WAMR_IWASM)/aot/arch/aot_reloc_x86_64.c
    WAMR_ARCH_DEFS  := -DWASM_HAVE_MREMAP=0
  endif
else
WAMR_PLATFORM_INIT := $(WAMR_SHARED)/platform/linux/platform_init.c
WAMR_PLATFORM_HDR  := $(WAMR_SHARED)/platform/linux
WAMR_ARCH_RELOC    := $(WAMR_IWASM)/aot/arch/aot_reloc_x86_64.c
WAMR_ARCH_DEFS     := -DWASM_HAVE_MREMAP=1 -D_GNU_SOURCE
WAMR_MREMAP_SRC    :=
  ifeq ($(shell uname -m),aarch64)
    WAMR_ARCH_RELOC := $(WAMR_IWASM)/aot/arch/aot_reloc_aarch64.c
    WAMR_ARCH_DEFS  += -DBUILD_TARGET=\"AARCH64\"
  endif
endif

ifdef COSMO
# Cosmopolitan: use WAMR's native cosmo platform support
WAMR_PLATFORM_INIT := $(WAMR_SHARED)/platform/cosmopolitan/platform_init.c
WAMR_PLATFORM_HDR  := $(WAMR_SHARED)/platform/cosmopolitan
WAMR_ARCH_RELOC    := $(WAMR_IWASM)/aot/arch/aot_reloc_x86_64.c
WAMR_ARCH_DEFS     := -DWASM_HAVE_MREMAP=0
WAMR_MREMAP_SRC    := $(WAMR_SHARED)/platform/common/memory/mremap.c
endif

WAMR_SRCS := \
	$(WAMR_IWASM)/common/wasm_application.c \
	$(WAMR_IWASM)/common/wasm_blocking_op.c \
	$(WAMR_IWASM)/common/wasm_c_api.c \
	$(WAMR_IWASM)/common/wasm_exec_env.c \
	$(WAMR_IWASM)/common/wasm_loader_common.c \
	$(WAMR_IWASM)/common/wasm_memory.c \
	$(WAMR_IWASM)/common/wasm_native.c \
	$(WAMR_IWASM)/common/wasm_runtime_common.c \
	$(WAMR_IWASM)/common/wasm_shared_memory.c \
	$(WAMR_IWASM)/interpreter/wasm_interp_fast.c \
	$(WAMR_IWASM)/interpreter/wasm_loader.c \
	$(WAMR_IWASM)/interpreter/wasm_runtime.c \
	$(WAMR_IWASM)/aot/aot_intrinsic.c \
	$(WAMR_IWASM)/aot/aot_loader.c \
	$(WAMR_IWASM)/aot/aot_runtime.c \
	$(WAMR_ARCH_RELOC) \
	$(WAMR_SHARED)/platform/common/posix/posix_blocking_op.c \
	$(WAMR_SHARED)/platform/common/posix/posix_malloc.c \
	$(WAMR_SHARED)/platform/common/posix/posix_memmap.c \
	$(WAMR_SHARED)/platform/common/posix/posix_sleep.c \
	$(WAMR_SHARED)/platform/common/posix/posix_thread.c \
	$(WAMR_SHARED)/platform/common/posix/posix_time.c \
	$(WAMR_PLATFORM_INIT) \
	$(WAMR_SHARED)/mem-alloc/mem_alloc.c \
	$(WAMR_SHARED)/mem-alloc/ems/ems_alloc.c \
	$(WAMR_SHARED)/mem-alloc/ems/ems_gc.c \
	$(WAMR_SHARED)/mem-alloc/ems/ems_hmu.c \
	$(WAMR_SHARED)/mem-alloc/ems/ems_kfc.c \
	$(WAMR_MREMAP_SRC) \
	$(WAMR_SHARED)/utils/bh_assert.c \
	$(WAMR_SHARED)/utils/bh_bitmap.c \
	$(WAMR_SHARED)/utils/bh_common.c \
	$(WAMR_SHARED)/utils/bh_hashmap.c \
	$(WAMR_SHARED)/utils/bh_leb128.c \
	$(WAMR_SHARED)/utils/bh_list.c \
	$(WAMR_SHARED)/utils/bh_log.c \
	$(WAMR_SHARED)/utils/bh_queue.c \
	$(WAMR_SHARED)/utils/bh_vector.c \
	$(WAMR_SHARED)/utils/runtime_timer.c \
	$(WAMR_IWASM)/libraries/shared-heap/shared_heap_wrapper.c

# Flatten WAMR paths to build/ (replace / with _ in the subpath)
WAMR_OBJS := $(patsubst $(WAMR_DIR)/%.c,$(BUILDDIR)/wamr_%.o,$(WAMR_SRCS))

# Native invoker: arm64 macOS needs assembly version (generic C invoker
# mangles arguments on arm64 calling convention). Other platforms use generic.
WAMR_INVOKE_OBJ := $(BUILDDIR)/wamr_invoke_native.o
ifeq ($(UNAME_S),Darwin)
  ifneq ($(shell uname -m),x86_64)
    WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_osx_universal.s
    WAMR_INVOKE_FLAGS := -DBH_PLATFORM_DARWIN -DWASM_ENABLE_SIMD=1
  else
    WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_general.c
  endif
else
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_general.c
endif
WAMR_OBJS += $(WAMR_INVOKE_OBJ)

WAMR_CFLAGS := -std=c11 -O2 -w $(WAMR_ARCH_DEFS) \
	-DWASM_ENABLE_INTERP=1 \
	-DWASM_ENABLE_FAST_INTERP=1 \
	-DWASM_ENABLE_AOT=1 \
	-DWASM_ENABLE_WASI=0 \
	-DWASM_ENABLE_MULTI_MODULE=0 \
	-DWASM_ENABLE_THREAD_MGR=0 \
	-DWASM_ENABLE_LIBC_BUILTIN=0 \
	-DWASM_ENABLE_LIBC_WASI=0 \
	-DWASM_ENABLE_BULK_MEMORY=1 \
	-DWASM_ENABLE_BULK_MEMORY_OPT=1 \
	-DWASM_ENABLE_REF_TYPES=0 \
	-DWASM_ENABLE_SIMD=1 \
	-DWASM_ENABLE_MINI_LOADER=0 \
	-DWASM_ENABLE_SHARED_MEMORY=0 \
	-DWASM_ENABLE_SHARED_HEAP=1 \
	-DWASM_ENABLE_MEMORY64=1 \
	-DWASM_ENABLE_MEMORY_PROFILING=0 \
	-DWASM_ENABLE_MEMORY_TRACING=0 \
	-DWASM_ENABLE_PERF_PROFILING=0 \
	-DWASM_ENABLE_GC=0 \
	-DWASM_ENABLE_STRINGREF=0 \
	-DWASM_ENABLE_EXCE_HANDLING=0 \
	-DWASM_ENABLE_TAGS=0 \
	-DWASM_ENABLE_INSTRUCTION_METERING=1 \
	-DBH_MALLOC=wasm_runtime_malloc \
	-DBH_FREE=wasm_runtime_free \
	-I$(WAMR_IWASM)/include \
	-I$(WAMR_IWASM)/common \
	-I$(WAMR_IWASM)/interpreter \
	-I$(WAMR_IWASM)/aot \
	-I$(WAMR_SHARED)/include \
	-I$(WAMR_SHARED)/platform/include \
	-I$(WAMR_SHARED)/platform/common/posix \
	-I$(WAMR_PLATFORM_HDR) \
	-I$(WAMR_SHARED)/utils \
	-I$(WAMR_SHARED)/mem-alloc \
	-I$(WAMR_CORE)

# WAMR include path for hull source
WAMR_INC := -I$(WAMR_IWASM)/include

else
# WASM disabled
WAMR_OBJS :=
WAMR_INC  :=
endif

# ── wgpu-native (GPU compute — optional) ─────────────────────────
#
# Optional GPU compute backend. Disabled by default.
# Enable with: make HL_ENABLE_GPU=1
#   - Auto-detects vendor/wgpu/libwgpu_native.a if present
#   - Or specify: make HL_ENABLE_GPU=1 WGPU_LIB_DIR=/path/to/lib
#   - Fetch automatically: make fetch-wgpu && make HL_ENABLE_GPU=1

HL_ENABLE_GPU ?= 0

ifeq ($(HL_ENABLE_GPU),1)
  CFLAGS += -DHL_ENABLE_GPU -I$(VENDDIR)/wgpu
  # Auto-detect vendor/wgpu if WGPU_LIB_DIR not specified
  ifndef WGPU_LIB_DIR
    ifneq (,$(wildcard $(VENDDIR)/wgpu/libwgpu_native.a))
      WGPU_LIB_DIR := $(VENDDIR)/wgpu
    else
      $(error HL_ENABLE_GPU=1 requires wgpu-native. Run: make fetch-wgpu)
    endif
  endif
  WGPU_LIB := $(WGPU_LIB_DIR)/libwgpu_native.a
  ifeq ($(UNAME_S),Darwin)
    WGPU_FRAMEWORKS := -framework Metal -framework QuartzCore -framework CoreGraphics -framework Foundation
  else
    WGPU_FRAMEWORKS := -lvulkan
  endif
  # GPU is not compatible with Cosmopolitan builds
  ifdef COSMO
    $(error GPU compute is not compatible with Cosmopolitan builds)
  endif
  WORKER_GPU_OBJ := $(BUILDDIR)/worker_gpu.o
else
  WORKER_GPU_OBJ :=
  WGPU_LIB :=
  WGPU_FRAMEWORKS :=
endif

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
ASYNC_OBJ      := $(BUILDDIR)/hull_async.o
COMPRESS_OBJ   := $(BUILDDIR)/hull_compress.o
MINIZ_OBJ      := $(BUILDDIR)/miniz.o
WORKER_DB_OBJ  := $(BUILDDIR)/worker_db.o
WORKER_WASM_OBJ := $(BUILDDIR)/worker_wasm.o
MANIFEST_OBJ   := $(BUILDDIR)/manifest.o
SANDBOX_OBJ    := $(BUILDDIR)/sandbox.o

# Test-specific objects (single runtime — avoids pulling Lua into JS tests and vice versa)
MANIFEST_JS_OBJ  := $(BUILDDIR)/manifest_js_only.o
MANIFEST_LUA_OBJ := $(BUILDDIR)/manifest_lua_only.o
CAP_TEST_JS_OBJ  := $(BUILDDIR)/cap_test_js_only.o
TOOL_OBJ       := $(BUILDDIR)/tool.o
SIG_OBJ        := $(BUILDDIR)/signature.o
STATIC_OBJ     := $(BUILDDIR)/hull_static.o
BUILD_ASSET_OBJ      := $(BUILDDIR)/build_assets.o
BUILD_ASSET_STUB_OBJ := $(BUILDDIR)/build_assets_stub.o
MIGRATE_OBJ    := $(BUILDDIR)/migrate.o
VFS_OBJ        := $(BUILDDIR)/vfs.o
AGENT_LIB_OBJ := $(BUILDDIR)/agent_lib.o
AGENT_API_OBJ  := $(BUILDDIR)/agent_api.o
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

# Generate per-file xxd rules (avoids % matching directory separators)
define STDLIB_LUA_RULE
$(call stdlib_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(STDLIB_LUA_FILES),$(eval $(call STDLIB_LUA_RULE,$(f))))

STDLIB_LUA_XXD_HDRS := $(STDLIB_LUA_HDRS)

# ── JS stdlib embedding (xxd) ────────────────────────────────────────
#
# Mirror of the Lua pipeline for stdlib/js/**/*.js files.
# Module names use colon separator: stdlib/js/hull/verify.js → hull:verify

STDLIB_JS_FILES := $(shell find stdlib/js -name '*.js' -not -path '*/tests/*' 2>/dev/null)

# Flatten path: stdlib/js/hull/verify.js → build/stdlib_js_hull_verify.h
stdlib_js_hdr = $(BUILDDIR)/$(subst /,_,$(patsubst stdlib/%.js,stdlib_%.h,$(1)))
STDLIB_JS_HDRS := $(foreach f,$(STDLIB_JS_FILES),$(call stdlib_js_hdr,$(f)))

define STDLIB_JS_RULE
$(call stdlib_js_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(STDLIB_JS_FILES),$(eval $(call STDLIB_JS_RULE,$(f))))

STDLIB_JS_XXD_HDRS := $(STDLIB_JS_HDRS)

# ── Context doc embedding (xxd) ───────────────────────────────────────
#
# Markdown docs in stdlib/context/*.md are embedded for hull agent context.
# Names use context: prefix: stdlib/context/auth.md → context:auth

CONTEXT_FILES := $(wildcard stdlib/context/*.md)

context_hdr = $(BUILDDIR)/$(subst /,_,$(patsubst stdlib/context/%.md,context_%.h,$(1)))
CONTEXT_HDRS := $(foreach f,$(CONTEXT_FILES),$(call context_hdr,$(f)))

define CONTEXT_RULE
$(call context_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(CONTEXT_FILES),$(eval $(call CONTEXT_RULE,$(f))))

CONTEXT_XXD_HDRS := $(CONTEXT_HDRS)

# ── Unified stdlib registry (.c compiled once, linked by both runtimes) ──
#
# Merges Lua (dot names), JS (colon names), and context docs into
# a single hl_stdlib_entries[].
# Runtimes filter at load time: strchr(name, ':') → JS, else Lua.

STDLIB_REGISTRY_C := $(BUILDDIR)/stdlib_registry.c
STDLIB_REGISTRY_O := $(BUILDDIR)/stdlib_registry.o

$(STDLIB_REGISTRY_C): $(STDLIB_LUA_XXD_HDRS) $(STDLIB_JS_XXD_HDRS) $(CONTEXT_XXD_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated unified stdlib registry — do not edit */" > $@
	@for hdr in $(STDLIB_LUA_XXD_HDRS) $(STDLIB_JS_XXD_HDRS) $(CONTEXT_XXD_HDRS); do \
		echo "#include \"$$(basename $$hdr)\""; \
	done >> $@
	@echo "" >> $@
	@echo "#include \"hull/entry.h\"" >> $@
	@echo "const HlEntry hl_stdlib_entries[] = {" >> $@
	@( for f in $(STDLIB_LUA_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/lua/||; s|\.lua$$||; s|/|.|g'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(STDLIB_JS_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/js/||; s|\.js$$||; s|/|:|g'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(CONTEXT_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/context/||; s|\.md$$||'); \
		echo "context:$$modname	    { \"context:$$modname\", $${varname}, sizeof($${varname}) },"; \
	done ) | LC_ALL=C sort | cut -f2- >> $@
	@echo "    { 0, 0, 0 }" >> $@
	@echo "};" >> $@

$(STDLIB_REGISTRY_O): $(STDLIB_REGISTRY_C) | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -I$(INCDIR) -I$(BUILDDIR) -c -o $@ $<

# ── App code embedding (xxd) ─────────────────────────────────────────
#
# When APP_DIR is set (e.g. make APP_DIR=myapp), all app files are
# embedded into a single hl_app_entries[] array using xxd.
# Naming conventions:
#   .lua modules:    "./path" (no ext)       e.g. "./routes/users"
#   .js modules:     "./path.js"             e.g. "./app.js"
#   .json data:      "./path.json"           e.g. "./locales/en.json"
#   templates:       "templates/path"        e.g. "templates/base.html"
#   static files:    "static/path"           e.g. "static/style.css"
#   migrations:      "migrations/path"       e.g. "migrations/001_init.sql"

APP_ENTRIES_DEFAULT_OBJ := $(BUILDDIR)/app_entries_default.o
APP_DIR ?=
APP_EXTRA_OBJS := $(APP_ENTRIES_DEFAULT_OBJ)
ifneq ($(APP_DIR),)
APP_LUA_FILES := $(shell find $(APP_DIR) -name '*.lua' -not -path '*/tests/*' 2>/dev/null)
APP_JS_FILES := $(shell find $(APP_DIR) -name '*.js' -not -path '*/tests/*' -not -path '*/static/*' -not -path '*/node_modules/*' 2>/dev/null)
APP_JSON_FILES := $(shell find $(APP_DIR) -name '*.json' -not -path '*/tests/*' -not -path '*/static/*' -not -path '*/templates/*' 2>/dev/null)
APP_TPL_FILES := $(shell find $(APP_DIR)/templates -name '*.html' 2>/dev/null)
APP_STATIC_FILES := $(shell find $(APP_DIR)/static -type f 2>/dev/null)
APP_MIGRATION_FILES := $(shell find $(APP_DIR)/migrations -name '*.sql' 2>/dev/null | sort)
APP_COMPUTE_FILES := $(shell find $(APP_DIR)/compute \( -name '*.wasm' -o -name '*.aot.*' \) 2>/dev/null)

# xxd header paths per file type
app_lua_hdr = $(BUILDDIR)/app_lua_$(subst /,_,$(patsubst $(APP_DIR)/%.lua,%.h,$(1)))
app_js_hdr = $(BUILDDIR)/app_js_$(subst /,_,$(patsubst $(APP_DIR)/%.js,%.h,$(1)))
app_json_hdr = $(BUILDDIR)/app_json_$(subst /,_,$(patsubst $(APP_DIR)/%.json,%.h,$(1)))
app_tpl_hdr = $(BUILDDIR)/app_tpl_$(subst /,_,$(patsubst $(APP_DIR)/templates/%.html,%.h,$(1)))
app_static_hdr = $(BUILDDIR)/app_static_$(subst /,_,$(patsubst $(APP_DIR)/static/%,%.h,$(1)))
app_migration_hdr = $(BUILDDIR)/app_mig_$(subst /,_,$(patsubst $(APP_DIR)/migrations/%,%.h,$(1)))
app_compute_hdr = $(BUILDDIR)/app_compute_$(subst /,_,$(patsubst $(APP_DIR)/compute/%,%.h,$(1)))

APP_LUA_HDRS := $(foreach f,$(APP_LUA_FILES),$(call app_lua_hdr,$(f)))
APP_JS_HDRS := $(foreach f,$(APP_JS_FILES),$(call app_js_hdr,$(f)))
APP_JSON_HDRS := $(foreach f,$(APP_JSON_FILES),$(call app_json_hdr,$(f)))
APP_TPL_HDRS := $(foreach f,$(APP_TPL_FILES),$(call app_tpl_hdr,$(f)))
APP_STATIC_HDRS := $(foreach f,$(APP_STATIC_FILES),$(call app_static_hdr,$(f)))
APP_MIGRATION_HDRS := $(foreach f,$(APP_MIGRATION_FILES),$(call app_migration_hdr,$(f)))
APP_COMPUTE_HDRS := $(foreach f,$(APP_COMPUTE_FILES),$(call app_compute_hdr,$(f)))

# xxd rules for each file type
define APP_LUA_RULE
$(call app_lua_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(APP_LUA_FILES),$(eval $(call APP_LUA_RULE,$(f))))

define APP_JS_RULE
$(call app_js_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(APP_JS_FILES),$(eval $(call APP_JS_RULE,$(f))))

define APP_JSON_RULE
$(call app_json_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(APP_JSON_FILES),$(eval $(call APP_JSON_RULE,$(f))))

define APP_TPL_RULE
$(call app_tpl_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(APP_TPL_FILES),$(eval $(call APP_TPL_RULE,$(f))))

define APP_STATIC_RULE
$(call app_static_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(APP_STATIC_FILES),$(eval $(call APP_STATIC_RULE,$(f))))

define APP_MIGRATION_RULE
$(call app_migration_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(APP_MIGRATION_FILES),$(eval $(call APP_MIGRATION_RULE,$(f))))

define APP_COMPUTE_RULE
$(call app_compute_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(APP_COMPUTE_FILES),$(eval $(call APP_COMPUTE_RULE,$(f))))

APP_ALL_XXD_HDRS := $(APP_LUA_HDRS) $(APP_JS_HDRS) $(APP_JSON_HDRS) $(APP_TPL_HDRS) $(APP_STATIC_HDRS) $(APP_MIGRATION_HDRS) $(APP_COMPUTE_HDRS)

APP_REGISTRY_C := $(BUILDDIR)/app_registry.c
APP_REGISTRY_O := $(BUILDDIR)/app_registry.o

$(APP_REGISTRY_C): $(APP_ALL_XXD_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated unified app registry — do not edit */" > $@
	@for hdr in $(APP_ALL_XXD_HDRS); do \
		echo "#include \"$$(basename $$hdr)\""; \
	done >> $@
	@echo "" >> $@
	@echo "#include \"hull/entry.h\"" >> $@
	@echo "const HlEntry hl_app_entries[] = {" >> $@
	@( for f in $(APP_LUA_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^$(APP_DIR)/||; s|\.lua$$||'); \
		echo "./$$modname	    { \"./$$modname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(APP_JS_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^$(APP_DIR)/||'); \
		echo "./$$modname	    { \"./$$modname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(APP_JSON_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^$(APP_DIR)/||'); \
		echo "./$$modname	    { \"./$$modname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(APP_TPL_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		tplname=$$(echo "$$f" | sed 's|^$(APP_DIR)/||'); \
		echo "$$tplname	    { \"$$tplname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(APP_STATIC_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		staticname=$$(echo "$$f" | sed 's|^$(APP_DIR)/||'); \
		echo "$$staticname	    { \"$$staticname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(APP_MIGRATION_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		migname=$$(echo "$$f" | sed 's|^$(APP_DIR)/||'); \
		echo "$$migname	    { \"$$migname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(APP_COMPUTE_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		computename=$$(echo "$$f" | sed 's|^$(APP_DIR)/||'); \
		echo "$$computename	    { \"$$computename\", $${varname}, sizeof($${varname}) },"; \
	done ) | LC_ALL=C sort | cut -f2- >> $@
	@echo "    { 0, 0, 0 }" >> $@
	@echo "};" >> $@

$(APP_REGISTRY_O): $(APP_REGISTRY_C) | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -I$(INCDIR) -I$(BUILDDIR) -c -o $@ $<

APP_EXTRA_OBJS := $(APP_REGISTRY_O)
endif

# App entries default (empty array — used when no APP_DIR)
$(APP_ENTRIES_DEFAULT_OBJ): $(SRCDIR)/hull/app_entries_default.c $(INCDIR)/hull/entry.h | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -I$(INCDIR) -c -o $@ $<

# ── Include paths ───────────────────────────────────────────────────

INCLUDES := -I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(KEEL_INC) -I$(KEEL_DIR)/vendor/llhttp -I$(MBEDTLS_DIR)/include -I$(SQLITE_DIR) -I$(LOG_DIR) -I$(SH_ARENA_DIR) -I$(SH_JSON_DIR) -I$(TWEETNACL_DIR) -I$(BUILDDIR) $(WAMR_INC)

# ── Targets ─────────────────────────────────────────────────────────

.PHONY: all clean test debug msan e2e e2e-build e2e-http e2e-sandbox e2e-examples e2e-migrate e2e-templates e2e-agent e2e-context e2e-mcp e2e-agent-api hull-test-examples self-build check analyze cppcheck bench bench-template bench-wasm bench-gpu wamrc coverage lint-lua lint-js lint platform platform-cosmo

all: $(BUILDDIR)/hull

# Platform static library — everything except entry.o and build_assets.o
# Used by `hull build` to produce standalone app binaries.
# Exports hull_main() (subcommand dispatch + server logic).
PLATFORM_OBJS := $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(MANIFEST_OBJ) $(SANDBOX_OBJ) $(SIG_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(MAIN_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_STUB_OBJ) $(STDLIB_REGISTRY_O) $(WAMR_OBJS) $(VEND_OBJS) $(MBEDTLS_OBJS) \
	$(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(PLEDGE_OBJS)

PLATFORM_LIB := $(BUILDDIR)/libhull_platform.a

# Platform canary — embeds an integrity hash so the browser verifier can
# detect whether the Hull platform is actually present in the binary.
CANARY_C    := $(BUILDDIR)/platform_canary.c
CANARY_OBJ  := $(BUILDDIR)/platform_canary.o
CANARY_HASH := $(BUILDDIR)/platform_canary_hash

$(CANARY_C): $(PLATFORM_OBJS) | $(BUILDDIR)
	@hash=$$(cat $(sort $(PLATFORM_OBJS)) | $(SHA256CMD) | cut -d' ' -f1) && \
	echo "$$hash" > $(CANARY_HASH) && \
	bytes=$$(echo "$$hash" | fold -w2 | awk '{printf "%s0x%s",(NR>1?",":""),$$0}') && \
	printf '/* Auto-generated platform canary — do not edit */\n#include <stdint.h>\nconst struct { char magic[24]; uint8_t integrity[32]; } hl_platform_canary = {\n    "HULL_PLATFORM_CANARY",\n    {%s}\n};\n' "$$bytes" > $@

$(CANARY_OBJ): $(CANARY_C) | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -c -o $@ $<

$(PLATFORM_LIB): $(PLATFORM_OBJS) $(CANARY_OBJ) $(KEEL_LIB) | $(BUILDDIR)
	@rm -f $@
	$(AR) rcs $@ $(PLATFORM_OBJS) $(CANARY_OBJ)
	@# Merge keel objects into the platform archive
	@tmpdir=$$(mktemp -d) && \
		cd $$tmpdir && \
		$(AR) x $(CURDIR)/$(KEEL_LIB) && \
		$(AR) rcs $(CURDIR)/$@ *.o && \
		rm -rf $$tmpdir
	@# Record the CC used so hull build can auto-detect
	@echo "$(CC)" > $(BUILDDIR)/platform_cc

platform: $(PLATFORM_LIB)

# Multi-arch cosmo platform: build x86_64 and aarch64 archives
COSMO_STAGE := .cosmo_staging

platform-cosmo:
	@rm -rf $(COSMO_STAGE) && mkdir -p $(COSMO_STAGE)
	@echo "=== Building x86_64-cosmo platform ==="
	$(MAKE) clean
	$(MAKE) -C $(KEEL_DIR) clean
	$(MAKE) platform CC=x86_64-unknown-cosmo-cc AR=x86_64-unknown-cosmo-ar
	cp $(BUILDDIR)/libhull_platform.a $(COSMO_STAGE)/libhull_platform.x86_64-cosmo.a
	cp $(BUILDDIR)/platform_canary_hash $(COSMO_STAGE)/platform_canary_hash.x86_64-cosmo
	@echo "=== Building aarch64-cosmo platform ==="
	$(MAKE) clean
	$(MAKE) -C $(KEEL_DIR) clean
	$(MAKE) platform CC=aarch64-unknown-cosmo-cc AR=aarch64-unknown-cosmo-ar
	cp $(BUILDDIR)/libhull_platform.a $(COSMO_STAGE)/libhull_platform.aarch64-cosmo.a
	cp $(BUILDDIR)/platform_canary_hash $(COSMO_STAGE)/platform_canary_hash.aarch64-cosmo
	$(MAKE) clean
	$(MAKE) -C $(KEEL_DIR) clean
	mkdir -p $(BUILDDIR)
	cp $(COSMO_STAGE)/* $(BUILDDIR)/
	echo "cosmocc" > $(BUILDDIR)/platform_cc
	rm -rf $(COSMO_STAGE)

# ── wamrc AOT compiler ──────────────────────────────────────────────
#
# Build the WAMR AOT compiler from vendor/wamr/wamr-compiler.
# Requires: cmake, LLVM (brew install llvm on macOS, apt install llvm on Linux).
# Output: build/wamrc
# Override LLVM path: make wamrc WAMRC_CMAKE_FLAGS="-DLLVM_DIR=/path/to/llvm/cmake"

WAMRC_BUILD_DIR := $(BUILDDIR)/wamrc-build

wamrc: | $(BUILDDIR)
	@echo "=== Building wamrc AOT compiler ==="
	@mkdir -p $(WAMRC_BUILD_DIR)
	@cd $(WAMRC_BUILD_DIR) && cmake $(CURDIR)/$(WAMR_DIR)/wamr-compiler \
		-DCMAKE_BUILD_TYPE=Release \
		-DWAMR_BUILD_WITH_CUSTOM_LLVM=1 \
		-DWASM_ENABLE_INSTRUCTION_METERING=1 \
		$(WAMRC_CMAKE_FLAGS) 2>&1 | tail -5
	@$(MAKE) -C $(WAMRC_BUILD_DIR) -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) 2>&1 | tail -3
	@cp $(WAMRC_BUILD_DIR)/wamrc $(BUILDDIR)/wamrc
	@echo "=== wamrc built: $(BUILDDIR)/wamrc ==="

# ── Embedded build assets (distribution builds only) ────────────────
# Build with: make EMBED_PLATFORM=1      (single-arch)
#             make EMBED_PLATFORM=cosmo  (multi-arch cosmo)
# This xxd's the platform .a + templates into build_assets.c
EMBED_PLATFORM ?=
EMBEDDED_TEMPLATES_H := $(BUILDDIR)/embedded_templates.h
EMBEDDED_PLATFORM_H := $(BUILDDIR)/embedded_platform.h

ifeq ($(EMBED_PLATFORM),cosmo)
# Multi-arch cosmo embedding — xxd both archives + metadata table
$(EMBEDDED_PLATFORM_H): $(BUILDDIR)/libhull_platform.x86_64-cosmo.a \
                         $(BUILDDIR)/libhull_platform.aarch64-cosmo.a | $(BUILDDIR)
	@echo "/* Auto-generated multi-arch — do not edit */" > $@
	xxd -i $(BUILDDIR)/libhull_platform.x86_64-cosmo.a | \
		sed 's/build_libhull_platform_x86_64_cosmo_a/hl_platform_x86_64_cosmo/g' >> $@
	xxd -i $(BUILDDIR)/libhull_platform.aarch64-cosmo.a | \
		sed 's/build_libhull_platform_aarch64_cosmo_a/hl_platform_aarch64_cosmo/g' >> $@
	@echo "" >> $@
	@echo "static const HlEmbeddedPlatform hl_embedded_platforms[] = {" >> $@
	@echo '    { "x86_64-cosmo", hl_platform_x86_64_cosmo, sizeof(hl_platform_x86_64_cosmo) },' >> $@
	@echo '    { "aarch64-cosmo", hl_platform_aarch64_cosmo, sizeof(hl_platform_aarch64_cosmo) },' >> $@
	@echo "    { NULL, NULL, 0 }" >> $@
	@echo "};" >> $@

$(EMBEDDED_TEMPLATES_H): templates/app_main.c templates/entry.h | $(BUILDDIR)
	@echo "/* Auto-generated — do not edit */" > $@
	@xxd -i templates/app_main.c | sed 's/templates_app_main_c/hl_embedded_app_main_c/g' >> $@
	@xxd -i templates/entry.h | sed 's/templates_entry_h/hl_embedded_entry_h/g' >> $@

CFLAGS += -DHL_BUILD_EMBEDDED -DHL_BUILD_EMBEDDED_MULTIARCH
$(BUILD_ASSET_OBJ): $(EMBEDDED_PLATFORM_H) $(EMBEDDED_TEMPLATES_H)

else ifneq ($(EMBED_PLATFORM),)
# Single-arch embedding (existing behavior)
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
$(BUILDDIR)/hull: $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(MANIFEST_OBJ) $(SANDBOX_OBJ) $(SIG_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_OBJ) $(MAIN_OBJ) $(ENTRY_OBJ) $(APP_EXTRA_OBJS) $(STDLIB_REGISTRY_O) $(WAMR_OBJS) $(VEND_OBJS) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(PLEDGE_OBJS) $(KEEL_LIB)
	$(CC) $(LDFLAGS) -o $@ $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(MANIFEST_OBJ) $(SANDBOX_OBJ) $(SIG_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_OBJ) $(MAIN_OBJ) $(ENTRY_OBJ) $(APP_EXTRA_OBJS) $(STDLIB_REGISTRY_O) $(WAMR_OBJS) $(VEND_OBJS) $(MBEDTLS_OBJS) \
		$(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(PLEDGE_OBJS) $(KEEL_LIB) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Capability sources
$(BUILDDIR)/cap_%.o: $(SRCDIR)/hull/cap/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Command module sources
$(BUILDDIR)/cmd_%.o: $(SRCDIR)/hull/commands/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# JS runtime sources
$(BUILDDIR)/js_%.o: $(SRCDIR)/hull/runtime/js/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Lua runtime sources
$(BUILDDIR)/lua_rt_%.o: $(SRCDIR)/hull/runtime/lua/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Hull allocator
$(ALLOC_OBJ): $(SRCDIR)/hull/alloc.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Hull async glue (runtime-agnostic HlAsyncCtx + callbacks)
$(ASYNC_OBJ): $(SRCDIR)/hull/async.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Hull compression helper
$(COMPRESS_OBJ): $(SRCDIR)/hull/compress.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# miniz (vendored compression library — compiled with relaxed warnings)
$(MINIZ_OBJ): $(MINIZ_DIR)/miniz.c | $(BUILDDIR)
	$(CC) -std=c11 -O2 -I$(MINIZ_DIR) -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -w -c -o $@ $<

# Worker DB (runtime-agnostic per-worker SQLite connections)
$(WORKER_DB_OBJ): $(SRCDIR)/hull/worker_db.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Worker WASM (runtime-agnostic WASM thread pool dispatch)
$(WORKER_WASM_OBJ): $(SRCDIR)/hull/worker_wasm.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(WAMR_CFLAGS) -c -o $@ $<

# Worker GPU (runtime-agnostic GPU thread pool dispatch)
ifeq ($(HL_ENABLE_GPU),1)
$(WORKER_GPU_OBJ): $(SRCDIR)/hull/worker_gpu.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
endif

# Manifest
$(MANIFEST_OBJ): $(SRCDIR)/hull/manifest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Manifest (JS-only, for test_js — excludes Lua extraction to avoid Lua link deps)
$(MANIFEST_JS_OBJ): $(SRCDIR)/hull/manifest.c | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA,$(CFLAGS)) $(INCLUDES) -c -o $@ $<

# cap/test.c (JS-only, for test_js — excludes Lua bindings to avoid Lua link deps)
$(CAP_TEST_JS_OBJ): $(SRCDIR)/hull/cap/test.c | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA,$(CFLAGS)) $(INCLUDES) -c -o $@ $<

# Manifest (Lua-only, for test_lua — excludes JS extraction to avoid QuickJS link deps)
$(MANIFEST_LUA_OBJ): $(SRCDIR)/hull/manifest.c | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -c -o $@ $<

# Sandbox (pledge/unveil enforcement)
$(SANDBOX_OBJ): $(SRCDIR)/hull/sandbox.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Signature verification
$(SIG_OBJ): $(SRCDIR)/hull/signature.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Static file serving middleware
$(STATIC_OBJ): $(SRCDIR)/hull/static.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Migration runner
$(MIGRATE_OBJ): $(SRCDIR)/hull/migrate.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Virtual filesystem (sorted entry lookup)
$(VFS_OBJ): $(SRCDIR)/hull/vfs.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Agent library (shared by CLI, MCP, HTTP endpoints)
$(AGENT_LIB_OBJ): $(SRCDIR)/hull/agent_lib.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Agent API (diagnostic HTTP endpoints)
$(AGENT_API_OBJ): $(SRCDIR)/hull/agent_api.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Tool mode (keygen, build, verify, etc.)
$(TOOL_OBJ): $(SRCDIR)/hull/tool.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Build assets (embedded platform lib — stub unless HL_BUILD_EMBEDDED=1)
$(BUILD_ASSET_OBJ): $(SRCDIR)/hull/build_assets.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Build assets stub (no-op stubs for platform archive — satisfies cap_tool.o refs)
$(BUILD_ASSET_STUB_OBJ): $(SRCDIR)/hull/build_assets_stub.c | $(BUILDDIR)
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

# sh_json (vendored, relaxed warnings)
$(SH_JSON_OBJ): $(SH_JSON_DIR)/sh_json.c | $(BUILDDIR)
	$(CC) $(SH_JSON_CFLAGS) -I$(SH_JSON_DIR) -I$(SH_ARENA_DIR) -c -o $@ $<

# TweetNaCl (vendored, relaxed warnings)
$(TWEETNACL_OBJ): $(TWEETNACL_DIR)/tweetnacl.c | $(BUILDDIR)
	$(CC) $(TWEETNACL_CFLAGS) -I$(TWEETNACL_DIR) -c -o $@ $<

# jart/pledge polyfill (vendored, Linux only, relaxed warnings)
# Flatten libc/calls/pledge.c → build/pledge_libc_calls_pledge.o
$(BUILDDIR)/pledge_%.o: $(PLEDGE_DIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(PLEDGE_CFLAGS) -c -o $@ $<

# WAMR (vendored, relaxed warnings)
# Flatten vendor/wamr/core/iwasm/... → build/wamr_core_iwasm_...
ifeq ($(HL_ENABLE_WASM),1)
$(BUILDDIR)/wamr_%.o: $(WAMR_DIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(WAMR_CFLAGS) -c -o $@ $<
$(WAMR_INVOKE_OBJ): $(WAMR_INVOKE_SRC) | $(BUILDDIR)
	$(CC) $(WAMR_CFLAGS) $(WAMR_INVOKE_FLAGS) -c -o $@ $<
endif

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
TEST_COMMON_DEPS := $(TEST_CAP_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(VFS_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(KEEL_LIB)
TEST_COMMON_LIBS := $(TEST_CAP_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(VFS_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) $(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Capability tests (tests/hull/cap/)
$(BUILDDIR)/test_%: $(TESTDIR)/hull/cap/test_%.c $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_COMMON_LIBS)

# Top-level tests (tests/hull/)
$(BUILDDIR)/test_parse_size: $(TESTDIR)/hull/test_parse_size.c $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_COMMON_LIBS)

# JS runtime test — needs QuickJS + JS runtime objects + manifest (JS-only to avoid Lua link deps)
$(BUILDDIR)/test_js: $(TESTDIR)/hull/runtime/js/test_js.c $(TEST_COMMON_DEPS) $(MANIFEST_JS_OBJ) $(CAP_TEST_JS_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(VFS_OBJ) $(JS_RT_OBJS) $(QJS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(CAP_TEST_JS_OBJ) $(JS_RT_OBJS) $(MANIFEST_JS_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(VFS_OBJ) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(WAMR_OBJS) $(QJS_OBJS) \
		$(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Lua runtime test — needs Lua + Lua runtime objects + manifest (Lua-only) + cap_tool + build_assets
$(BUILDDIR)/test_lua: $(TESTDIR)/hull/runtime/lua/test_lua.c $(TEST_COMMON_DEPS) $(CAP_TOOL_OBJ) $(BUILD_ASSET_OBJ) $(MANIFEST_LUA_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(VFS_OBJ) $(LUA_RT_OBJS) $(LUA_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(CAP_TOOL_OBJ) $(BUILD_ASSET_OBJ) $(LUA_RT_OBJS) $(MANIFEST_LUA_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(VFS_OBJ) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(WAMR_OBJS) $(LUA_OBJS) \
		$(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Tool hardening test — cap/tool.c compiled without runtime flags (self-contained C functions)
CAP_TOOL_NONE_OBJ := $(BUILDDIR)/cap_tool_none.o
$(CAP_TOOL_NONE_OBJ): $(SRCDIR)/hull/cap/tool.c | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/test_tool: $(TESTDIR)/hull/cap/test_tool.c $(CAP_TOOL_NONE_OBJ) $(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ) | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(CAP_TOOL_NONE_OBJ) $(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ)

# Command dispatcher test — needs full command set (symbol resolution for command table)
$(BUILDDIR)/test_dispatch: $(TESTDIR)/hull/commands/test_dispatch.c $(CMD_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(TOOL_OBJ) $(SANDBOX_OBJ) $(SIG_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(TEST_COMMON_DEPS) $(RT_OBJS) $(VEND_OBJS) $(MANIFEST_OBJ) $(BUILD_ASSET_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(PLEDGE_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(CMD_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(TOOL_OBJ) $(SANDBOX_OBJ) $(SIG_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) \
		$(TEST_CAP_OBJS) $(RT_OBJS) $(MANIFEST_OBJ) $(BUILD_ASSET_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(WAMR_OBJS) $(VEND_OBJS) \
		$(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(PLEDGE_OBJS) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Signature verification test — needs crypto + app_entries_default + vfs
$(BUILDDIR)/test_signature: $(TESTDIR)/hull/test_signature.c $(SIG_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(SIG_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(TEST_COMMON_LIBS)

# Static file serving test — needs static middleware + vfs + keel
$(BUILDDIR)/test_static: $(TESTDIR)/hull/test_static.c $(STATIC_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(STATIC_OBJ) $(TEST_COMMON_LIBS)

# VFS test — standalone module, no runtime deps
$(BUILDDIR)/test_vfs: $(TESTDIR)/hull/test_vfs.c $(VFS_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(VFS_OBJ)

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
                 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5
LOG_CFLAGS := -std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer \
              -DLOG_USE_COLOR
SH_ARENA_CFLAGS := -std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer
SH_JSON_CFLAGS := -std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer
TWEETNACL_CFLAGS := -std=c11 -O1 -w -fsanitize=memory,undefined -fno-omit-frame-pointer
# Re-add runtime defines (the := above clobbers earlier += additions)
ifeq ($(RUNTIME),js)
  CFLAGS += -DHL_ENABLE_JS
else ifeq ($(RUNTIME),lua)
  CFLAGS += -DHL_ENABLE_LUA
else
  CFLAGS += -DHL_ENABLE_JS -DHL_ENABLE_LUA
endif
ifeq ($(HL_ENABLE_WASM),1)
  CFLAGS += -DHL_ENABLE_WASM
endif
ifeq ($(HL_ENABLE_GPU),1)
  CFLAGS += -DHL_ENABLE_GPU -I$(VENDDIR)/wgpu
endif
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

e2e-http: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_http.sh

e2e-sandbox: $(BUILDDIR)/hull
	sh tests/e2e_sandbox.sh

e2e-examples: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_examples.sh

e2e-migrate: $(BUILDDIR)/hull
	sh tests/e2e_migrate.sh

e2e-templates: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_templates.sh

e2e-agent: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_agent.sh

e2e-context: $(BUILDDIR)/hull
	sh tests/e2e_context.sh

e2e-mcp: $(BUILDDIR)/hull
	sh tests/e2e_mcp.sh

e2e-agent-api: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_agent_api.sh

e2e-compute: $(BUILDDIR)/hull
	sh tests/e2e_compute.sh

hull-test-examples: $(BUILDDIR)/hull
	@for dir in examples/hello examples/rest_api examples/bench_db examples/auth \
	            examples/jwt_api examples/crud_with_auth examples/middleware examples/webhooks \
	            examples/todo examples/timers; do \
		echo "=== hull test $$dir ===" && \
		output=$$($(BUILDDIR)/hull test "$$dir" 2>&1; true) && \
		echo "$$output" && \
		if echo "$$output" | grep -qE "[0-9]+ failed"; then exit 1; fi; \
	done

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
	$(MAKE) $(VEND_OBJS) $(MBEDTLS_OBJS) $(MINIZ_OBJ) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(PLEDGE_OBJS) $(WAMR_OBJS) $(KEEL_LIB)
	scan-build --status-bugs -disable-checker alpha.unix.Stream $(MAKE) $(CAP_OBJS) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(MAIN_OBJ) $(BUILDDIR)/hull

cppcheck:
	cppcheck --enable=all --inline-suppr \
		--suppress=missingIncludeSystem \
		--suppress=missingInclude \
		--suppress=unusedFunction \
		--suppress=checkersReport \
		--suppress=toomanyconfigs \
		--suppress=normalCheckLevelMaxBranches \
		--suppress=checkLevelNormal \
		--suppress=constParameterCallback \
		--suppress=constParameterPointer \
		--suppress=constVariablePointer \
		--suppress=staticFunction \
		--suppress=uninitvar:$(SRCDIR)/hull/runtime/lua/bindings.c \
		--suppress=unusedLabelConfiguration:$(SRCDIR)/hull/main.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/agent_lib.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/cap/wasm.c \
		--suppress=unusedStructMember:$(SRCDIR)/hull/agent_lib.c \
		--suppress=unusedVariable:$(SRCDIR)/hull/agent_lib.c \
		--suppress=variableScope:$(SRCDIR)/hull/cap/wasm.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/runtime/js/modules.c \
		--suppress=unmatchedSuppression \
		--suppress='*:$(QJS_DIR)/*' \
		--suppress='*:$(LUA_DIR)/*' \
		--suppress='*:$(SQLITE_DIR)/*' \
		--suppress='*:$(LOG_DIR)/*' \
		--error-exitcode=1 \
		-I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(SQLITE_DIR) -I$(KEEL_INC) \
		$(SRCDIR)/hull/main.c $(SRCDIR)/hull/alloc.c $(SRCDIR)/hull/static.c $(SRCDIR)/hull/agent_lib.c $(SRCDIR)/hull/agent_api.c $(SRCDIR)/hull/cap/*.c \
		$(SRCDIR)/hull/commands/*.c \
		$(SRCDIR)/hull/runtime/js/*.c $(SRCDIR)/hull/runtime/lua/*.c 2>&1

# ── Benchmark ──────────────────────────────────────────────────────

bench: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh bench/bench.sh

bench-template: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh bench/bench_template.sh

BENCH_WASM_SRCS := bench/wasm/bench_wasm.c \
	bench/wasm/workloads/compute_hash_native.c \
	bench/wasm/workloads/mem_histogram_native.c \
	bench/wasm/workloads/simd_dot_product_native.c \
	bench/wasm/workloads/simd_matmul_native.c

$(BUILDDIR)/bench_wasm: $(BENCH_WASM_SRCS) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -Ibench/wasm/workloads -o $@ \
		$(BENCH_WASM_SRCS) $(TEST_COMMON_LIBS)

bench-wasm: $(BUILDDIR)/bench_wasm
	$(BUILDDIR)/bench_wasm

# GPU vs WASM vs native benchmark (requires HL_ENABLE_GPU=1)
BENCH_GPU_ARCH := $(shell uname -m | sed 's/arm64/aarch64/')
BENCH_GPU_AOT  := bench/gpu/cosine.aot.$(BENCH_GPU_ARCH)

$(BENCH_GPU_AOT): bench/gpu/cosine.wasm $(BUILDDIR)/wamrc
	$(BUILDDIR)/wamrc -o $@ $<

$(BUILDDIR)/bench_gpu: bench/gpu/bench_gpu.c $(TEST_COMMON_DEPS) $(BENCH_GPU_AOT) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -Ibench/gpu -o $@ \
		bench/gpu/bench_gpu.c $(TEST_COMMON_LIBS)

bench-gpu: $(BUILDDIR)/bench_gpu
	$(BUILDDIR)/bench_gpu

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

# ── Dependency fetching ──────────────────────────────────────────────

# wgpu-native v27.0.4.0 — GPU compute backend
WGPU_VERSION := v27.0.4.0
WGPU_SHA256_macos_aarch64 := 15367c26fdbe6892db35007d39f3883593384e777360b70e6bd704cb5dedde53
WGPU_SHA256_macos_x86_64  := 660fe9be59b555ec1d7c839e5cf8b6c71762938af61ab444a7a58dd87970dba2
WGPU_SHA256_linux_x86_64  := 271481ef76fbf3ea09631a6079e9493636ecf813cd9c92306c44a1a452991ba1
WGPU_SHA256_linux_aarch64  := a2f22248200997b69373273b10d50a58164f6ed840877289f3e46bff317b134e

# SHA-256 helper (portable: macOS uses shasum, Linux uses sha256sum)
SHA256CMD := $(shell command -v sha256sum 2>/dev/null || echo "$(SHA256CMD)")

# Detect platform for wgpu-native download
WGPU_OS := $(shell uname -s | tr A-Z a-z | sed 's/darwin/macos/')
WGPU_ARCH := $(shell uname -m | sed 's/arm64/aarch64/')
WGPU_PLATFORM := $(WGPU_OS)-$(WGPU_ARCH)
WGPU_ZIP := wgpu-$(WGPU_PLATFORM)-release.zip
WGPU_URL := https://github.com/gfx-rs/wgpu-native/releases/download/$(WGPU_VERSION)/$(WGPU_ZIP)
WGPU_EXPECTED_SHA := $(WGPU_SHA256_$(subst -,_,$(WGPU_PLATFORM)))

.PHONY: fetch-wgpu fetch-cosmocc

fetch-wgpu:
	@if [ -f $(VENDDIR)/wgpu/libwgpu_native.a ]; then \
		echo "wgpu-native already present at $(VENDDIR)/wgpu/"; \
	else \
		echo "=== Fetching wgpu-native $(WGPU_VERSION) for $(WGPU_PLATFORM) ==="; \
		if [ -z "$(WGPU_EXPECTED_SHA)" ]; then \
			echo "ERROR: unsupported platform $(WGPU_PLATFORM)"; \
			echo "Supported: macos-aarch64, macos-x86_64, linux-x86_64, linux-aarch64"; \
			exit 1; \
		fi; \
		curl -sL -o /tmp/$(WGPU_ZIP) "$(WGPU_URL)"; \
		echo "Verifying SHA-256..."; \
		ACTUAL=$$($(SHA256CMD) /tmp/$(WGPU_ZIP) | cut -d' ' -f1); \
		if [ "$$ACTUAL" != "$(WGPU_EXPECTED_SHA)" ]; then \
			echo "ERROR: SHA-256 mismatch!"; \
			echo "  expected: $(WGPU_EXPECTED_SHA)"; \
			echo "  actual:   $$ACTUAL"; \
			rm -f /tmp/$(WGPU_ZIP); \
			exit 1; \
		fi; \
		echo "SHA-256 OK"; \
		mkdir -p $(VENDDIR)/wgpu; \
		unzip -o -j /tmp/$(WGPU_ZIP) "lib/libwgpu_native.a" -d $(VENDDIR)/wgpu/; \
		unzip -o -j /tmp/$(WGPU_ZIP) "include/webgpu/webgpu.h" -d $(VENDDIR)/wgpu/; \
		unzip -o -j /tmp/$(WGPU_ZIP) "include/webgpu/wgpu.h" -d $(VENDDIR)/wgpu/; \
		rm -f /tmp/$(WGPU_ZIP); \
		echo "=== wgpu-native $(WGPU_VERSION) installed to $(VENDDIR)/wgpu/ ==="; \
		ls -lh $(VENDDIR)/wgpu/libwgpu_native.a; \
	fi

# Cosmopolitan cosmocc 4.0.2 — portable C compiler
COSMOCC_VERSION := 4.0.2
COSMOCC_SHA256 := 85b8c37a406d862e656ad4ec14be9f6ce474c1b436b9615e91a55208aced3f44
COSMOCC_URL := https://cosmo.zip/pub/cosmocc/cosmocc-$(COSMOCC_VERSION).zip
COSMOCC_DIR ?= /opt/cosmo

fetch-cosmocc:
	@if command -v cosmocc >/dev/null 2>&1; then \
		echo "cosmocc already installed: $$(which cosmocc)"; \
	else \
		echo "=== Fetching cosmocc $(COSMOCC_VERSION) to $(COSMOCC_DIR) ==="; \
		curl -sL -o /tmp/cosmocc.zip "$(COSMOCC_URL)"; \
		echo "Verifying SHA-256..."; \
		ACTUAL=$$($(SHA256CMD) /tmp/cosmocc.zip | cut -d' ' -f1); \
		if [ "$$ACTUAL" != "$(COSMOCC_SHA256)" ]; then \
			echo "ERROR: SHA-256 mismatch!"; \
			echo "  expected: $(COSMOCC_SHA256)"; \
			echo "  actual:   $$ACTUAL"; \
			rm -f /tmp/cosmocc.zip; \
			exit 1; \
		fi; \
		echo "SHA-256 OK"; \
		mkdir -p $(COSMOCC_DIR); \
		unzip -q -o /tmp/cosmocc.zip -d $(COSMOCC_DIR); \
		rm -f /tmp/cosmocc.zip; \
		echo "=== cosmocc $(COSMOCC_VERSION) installed to $(COSMOCC_DIR)/bin/cosmocc ==="; \
		echo "Add to PATH: export PATH=$(COSMOCC_DIR)/bin:\$$PATH"; \
	fi

# ── Clean ───────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR)
	@$(MAKE) -s -C $(KEEL_DIR) clean 2>/dev/null || true
