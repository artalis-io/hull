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

# ── Version string ────────────────────────────────────────────────────
#
# Precedence (highest to lowest):
#   1. VERSION file in repo root (written by release workflow before build)
#   2. git describe --tags --always --dirty
#   3. Fallback literal "dev"
_VERSION_FILE := $(shell cat VERSION 2>/dev/null)
ifneq ($(_VERSION_FILE),)
  HL_VERSION := $(_VERSION_FILE)
else
  HL_VERSION := $(shell git describe --tags --always --dirty 2>/dev/null)
  ifeq ($(HL_VERSION),)
    HL_VERSION := dev
  endif
endif

CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
# -Wall/-Wextra already enable these implicitly. Promoting unused
# functions and variables to errors is the actual policy: dead code in
# Hull source must be deleted, not left to accrue. -Wunused-parameter
# stays a warning (not an error) because vendored static-inline headers
# (notably QuickJS) leak unused-parameter diagnostics into every Hull
# TU that includes them, and we can't edit the vendor source. Unused
# parameters in Hull code itself should be silenced with `(void)x;`.
CFLAGS  += -Werror=unused-function -Werror=unused-variable
LDFLAGS :=

# Header-dependency tracking. -MMD writes a sibling .d file listing every
# user header the .c included; -MP emits a phony rule for each header so
# deleting or renaming one doesn't break the build. The -include block at
# the bottom of this file ("Header-dependency replay") re-loads those .d
# files on subsequent builds so a header change invalidates exactly the
# right .o files. Applied to every CFLAGS variant via follow-up += blocks
# so the policy is uniform across Hull and vendor sources.
DEPFLAGS := -MMD -MP
CFLAGS   += $(DEPFLAGS)

ifndef COSMO
  # Stack protection + PIE for ASLR. PIE is the macOS default since
  # 10.7 — passing `-pie` to clang on Darwin emits a harmless
  # "argument unused during compilation" warning that pollutes every
  # link line. Only set it where the linker actually needs it.
  CFLAGS  += -fstack-protector-strong -fPIE
  ifneq ($(UNAME_S),Darwin)
    LDFLAGS += -pie
  endif

  ifndef DEBUG
    # _FORTIFY_SOURCE=3 requires glibc 2.34+ / gcc 12+ / clang 9+; on
    # older toolchains it emits a noisy warning and behaves as =2.
    # We intentionally leave the warning loud so stale CI is visible.
    CFLAGS += -D_FORTIFY_SOURCE=3
  endif

  # Linux-only linker hardening: RELRO + BIND_NOW + non-executable
  # stack. ld64 (macOS) rejects -z flags, so gate to Linux.
  ifeq ($(UNAME_S),Linux)
    CFLAGS  += -D_DEFAULT_SOURCE
    LDFLAGS += -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
  endif

  # (Earlier audit rounds added `-Wl,--build-id=none` here under the
  # wrong theory that Linux Build-ID was random. Reality: GNU ld's
  # default `--build-id=sha1` is content-addressed — identical inputs
  # produce identical Build-IDs. Keeping the default preserves the
  # Build-ID for debuggers/crash reporters without sacrificing
  # reproducibility. Same applies to macOS LC_UUID: deterministic given
  # same output path + same input content. See roadmap_next.md §0.2 for
  # the full investigation arc.)
endif

# Reproducibility: deterministic ar archives. Without this, ar embeds
# the mtime + uid + gid of each member, so `libhull_platform.a` and
# `libkeel.a` differ between builds and the final link inherits the
# delta. GNU ar accepts the `D` flag for this; BSD ar (macOS) respects
# the `ZERO_AR_DATE=1` env var. Exporting it covers both via the
# shared toolchain envelope; harmless when the tool already defaults
# to deterministic (modern binutils does, with
# --enable-deterministic-archives configured at distro level).
export ZERO_AR_DATE := 1

# Build mode
ifdef DEBUG
CFLAGS += -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
else
CFLAGS += -O2
endif

ifdef COVERAGE
# -fprofile-update=atomic prevents racy negative counts in multithreaded
# tests (cap/wasm.c, worker_db.c, etc.) which otherwise make geninfo
# fail with "Unexpected negative count" in the CI coverage job.
CFLAGS  += -g -O0 --coverage -fprofile-update=atomic
LDFLAGS += --coverage
endif

CFLAGS += -DHL_VERSION=\"$(HL_VERSION)\"

# SBOM: bake submodule commits AND describe-tags in at compile time.
# `git rev-parse` + `git describe` are called per-build; the result is
# embedded in the binary so `hull sbom` self-describes the actual
# vendored contents without needing the source tree at runtime. Falls
# back to "unknown" if git is unavailable. `describe --tags --always`
# gives a clean tag ("v2.0.0") when one exists, else "<tag>-N-g<sha>".
HULL_VENDOR_KEEL_COMMIT  := $(shell git -C vendor/keel rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
HULL_VENDOR_WAMR_COMMIT  := $(shell git -C vendor/wamr rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
HULL_VENDOR_TCC_COMMIT   := $(shell git -C vendor/tcc  rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
HULL_VENDOR_KEEL_VERSION := $(shell git -C vendor/keel describe --tags --always 2>/dev/null || echo unknown)
HULL_VENDOR_WAMR_VERSION := $(shell git -C vendor/wamr describe --tags --always 2>/dev/null || echo unknown)
HULL_VENDOR_TCC_VERSION  := $(shell git -C vendor/tcc  describe --tags --always 2>/dev/null || echo unknown)
CFLAGS += -DHULL_VENDOR_KEEL_COMMIT=\"$(HULL_VENDOR_KEEL_COMMIT)\"
CFLAGS += -DHULL_VENDOR_WAMR_COMMIT=\"$(HULL_VENDOR_WAMR_COMMIT)\"
CFLAGS += -DHULL_VENDOR_TCC_COMMIT=\"$(HULL_VENDOR_TCC_COMMIT)\"
CFLAGS += -DHULL_VENDOR_KEEL_VERSION=\"$(HULL_VENDOR_KEEL_VERSION)\"
CFLAGS += -DHULL_VENDOR_WAMR_VERSION=\"$(HULL_VENDOR_WAMR_VERSION)\"
CFLAGS += -DHULL_VENDOR_TCC_VERSION=\"$(HULL_VENDOR_TCC_VERSION)\"

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

# QuickJS vendored-snapshot version. Bump this — and only this —
# whenever vendor/quickjs/ changes. Both the vendored QuickJS build
# (CONFIG_VERSION, used by quickjs.c) and the Hull-side bytecode /
# template caches (QJS_TAG, used to derive cache keys) read from
# this single variable, so cache invalidation is automatic on a
# QuickJS upgrade.
QJS_VERSION := 2024-01-13

# QuickJS compiled with relaxed warnings (vendored code)
QJS_CFLAGS := -std=c11 -O2 -w -DCONFIG_VERSION=\"$(QJS_VERSION)\" \
              -DCONFIG_BIGNUM -D_GNU_SOURCE

# Hull-side code (bytecode/template caches) reads the same string via
# `include/hull/runtime/quickjs_tag.h`.
CFLAGS += -DHL_QJS_VERSION=\"$(QJS_VERSION)\"

# ── Lua 5.4 ──────────────────────────────────────────────────────────

LUA_DIR  := $(VENDDIR)/lua
LUA_SRCS := $(filter-out $(LUA_DIR)/lua.c $(LUA_DIR)/luac.c, \
             $(wildcard $(LUA_DIR)/*.c))
LUA_OBJS := $(patsubst $(LUA_DIR)/%.c,$(BUILDDIR)/lua_%.o,$(LUA_SRCS))

# Lua compiled with relaxed warnings (vendored code)
LUA_CFLAGS := -std=c11 -O2 -w -DLUA_USE_POSIX

# ── HTTP server / client — config flags ─────────────────────────────
#
# Declared here (early) because the Keel + mbedTLS sections below gate
# on $(HL_ENABLE_HTTP_ANY). Full prose docs are repeated at line 195
# (where they used to live) so anyone scrolling the build flags table
# also finds them.

HL_ENABLE_HTTP ?= 1

# Back-compat: HL_ENABLE_HTTP=0 forces both off; otherwise honour the
# granular flag defaults (both ?= 1 below).
ifeq ($(HL_ENABLE_HTTP),0)
HL_ENABLE_HTTP_SERVER ?= 0
HL_ENABLE_HTTP_CLIENT ?= 0
endif
HL_ENABLE_HTTP_SERVER ?= 1
HL_ENABLE_HTTP_CLIENT ?= 1

# CFLAGS macros: granular always defined; HL_ENABLE_HTTP (the legacy
# "any HTTP at all" gate) defined when either is on.
ifeq ($(HL_ENABLE_HTTP_SERVER),1)
CFLAGS += -DHL_ENABLE_HTTP_SERVER
endif
ifeq ($(HL_ENABLE_HTTP_CLIENT),1)
CFLAGS += -DHL_ENABLE_HTTP_CLIENT
endif
ifeq ($(HL_ENABLE_HTTP_SERVER)$(HL_ENABLE_HTTP_CLIENT),00)
HL_ENABLE_HTTP_ANY := 0
else
HL_ENABLE_HTTP_ANY := 1
CFLAGS += -DHL_ENABLE_HTTP
endif

# ── Keel (external library) ─────────────────────────────────────────

# Keel is included as a git submodule in vendor/keel. Dropped from the
# link only when both HTTP halves are off — Keel ships the HTTP client
# (used by HL_ENABLE_HTTP_CLIENT=1) and the HTTP server (used by
# HL_ENABLE_HTTP_SERVER=1) together; the linker dead-strips the half
# that isn't referenced.
KEEL_DIR   ?= $(VENDDIR)/keel
KEEL_INC   := $(KEEL_DIR)/include
ifeq ($(HL_ENABLE_HTTP_ANY),0)
KEEL_LIB   :=
else
KEEL_LIB   := $(KEEL_DIR)/libkeel.a
endif

# Build Keel with mbedTLS backend
# Keel now detects the cosmo toolchain natively from CC and handles
# poll backend, .aarch64/ archive creation, etc.
MINIZ_DIR  := $(VENDDIR)/miniz

ifneq ($(KEEL_LIB),)
$(KEEL_LIB): $(MBEDTLS_OBJS)
	$(MAKE) -C $(KEEL_DIR) CC=$(CC) AR=$(AR) \
		KEEL_TLS=mbedtls MBEDTLS_CONFIG_FILE=hull_config.h \
		KEEL_COMPRESS=miniz MINIZ_DIR=$(CURDIR)/$(MINIZ_DIR)
endif

# ── mbedTLS (vendored) ─────────────────────────────────────────────
#
# mbedTLS is used by both halves of the HTTP stack:
#   - HTTP server (HL_ENABLE_HTTP_SERVER=1): TLS termination via Keel's
#     KlTlsCtx in serve.c.
#   - HTTP client (HL_ENABLE_HTTP_CLIENT=1): HTTPS for http.fetch +
#     STARTTLS for SMTP send (cap/smtp.c).
# Dropped from the link only when BOTH are off (pure-compute build).

MBEDTLS_DIR    := $(VENDDIR)/mbedtls
MBEDTLS_SRCS   := $(wildcard $(MBEDTLS_DIR)/library/*.c)
ifeq ($(HL_ENABLE_HTTP_ANY),0)
MBEDTLS_OBJS   :=
else
MBEDTLS_OBJS   := $(patsubst $(MBEDTLS_DIR)/library/%.c,$(BUILDDIR)/mbed_%.o,$(MBEDTLS_SRCS))
endif
MBEDTLS_CFLAGS := -std=c11 -O2 -w \
	-I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library -I$(MBEDTLS_DIR) \
	-DMBEDTLS_CONFIG_FILE='"hull_config.h"'

$(BUILDDIR)/mbed_%.o: $(MBEDTLS_DIR)/library/%.c | $(BUILDDIR)
	$(CC) $(MBEDTLS_CFLAGS) -c -o $@ $<

# ── SQLite (embedded relational DB) — config flag ──────────────────
# On by default. Drop for pure-compute builds via:
#   make HL_ENABLE_DB=0
# When disabled, every `cap/db*.c`, `worker_db*`, `migrate*`, `mod_db.c`,
# `agent/db.c`, and SQLite itself are excluded from the build. Apps must
# avoid `db.*`, `migrate.*`, and any stdlib module that depends on
# SQLite (session, ratelimit, idempotency, outbox, inbox, rbac, search).

HL_ENABLE_DB ?= 1

ifeq ($(HL_ENABLE_DB),1)
CFLAGS += -DHL_ENABLE_DB
endif

# ── HTTP server / client — config flag reference ────────────────────
#
# Flag values are computed up top (above the Keel / mbedTLS sections
# which gate on them); this block documents them.
#
#   HL_ENABLE_HTTP_SERVER (default 1)
#     The inbound HTTP stack: serve.c (KlServer setup), routing, body
#     reader, WebSocket server (cap/ws), middleware, SSE, the in-process
#     test harness (cap/test, test_runner), and `hull dev/test/agent/mcp`
#     commands. Drops mod_{server,ws,sse,test} + routes/dispatch/timers/
#     bindings glue in both runtimes. Apps with HL_ENABLE_HTTP_SERVER=0
#     must use `app.main(fn)` and may not declare hull/http-server,
#     hull/web/ws-server, hull/web/ws-client, hull/web/sse, or any
#     hull/web/middleware/*.
#
#   HL_ENABLE_HTTP_CLIENT (default 1)
#     The outbound network stack: `http.fetch` (cap/http + cap/http_async),
#     SMTP send (cap/smtp), and the `hull update` command (which uses
#     Keel's HTTPS client). Drops mod_http + mod_smtp from both runtimes.
#     Apps with HL_ENABLE_HTTP_CLIENT=0 may not declare hull/http,
#     hull/smtp, or hull/email.
#
# Combined effects (binary sizes are arm64 Darwin, default build):
#
#   server  client  result                                   binary
#   1       1       Full HTTP — default Hull build.          ~5.0 MB
#   0       1       CLI + outbound HTTPS. No server stack;   ~4.9 MB
#                   http.fetch works, can hit https://*.
#                   Keel + mbedTLS still linked.
#   1       0       Inbound server, no outbound HTTP. Niche  ~5.0 MB
#                   — server-only apps that may not make
#                   outgoing HTTP calls.
#   0       0       Pure compute / CLI. No Keel, no mbedTLS. ~4.4 MB
#
# HL_ENABLE_HTTP back-compat:
#   Setting HL_ENABLE_HTTP=0 still works — it pins both
#   HL_ENABLE_HTTP_{SERVER,CLIENT} to 0. Setting HL_ENABLE_HTTP=1
#   (the default) leaves the granular flags at their own defaults
#   (both 1), so existing `make` invocations don't change behavior.
#
# Linker dependencies:
#   mbedTLS and libkeel.a are linked when EITHER server or client
#   is on (Keel ships both halves; the linker dead-strips the unused
#   side). The compile-time -DHL_ENABLE_HTTP macro stays defined in
#   that same case, so existing source guards continue to mean "any
#   HTTP at all" — granular guards (HL_ENABLE_HTTP_{SERVER,CLIENT})
#   are only used where the distinction matters.

# ── SQLite (vendored amalgamation) ─────────────────────────────────

SQLITE_DIR    := $(VENDDIR)/sqlite
ifeq ($(HL_ENABLE_DB),0)
SQLITE_OBJ    :=
else
SQLITE_OBJ    := $(BUILDDIR)/sqlite3.o
endif
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

# ── stb_image (image decode/encode) ──────────────────────────────────

STB_DIR     := $(VENDDIR)/stb
STB_OBJ     := $(BUILDDIR)/stb_impl.o
STB_CFLAGS  := -std=c11 -O2 -w

$(STB_OBJ): $(STB_DIR)/stb_impl.c | $(BUILDDIR)
	$(CC) $(STB_CFLAGS) -I$(STB_DIR) -c -o $@ $<

# ── Unicode tables (TUI cell-width lookup) ──────────────────────────
#
# vendor/unicode/eaw.h is checked in (~28 KB) and included by
# src/hull/cap/tui_width.c via "unicode/eaw.h". The Unicode data
# files (EastAsianWidth.txt + UnicodeData.txt) and the generator
# (gen.lua) live alongside it; `make fetch-unicode` refreshes the
# data and regenerates the header.

UNICODE_DIR := $(VENDDIR)/unicode

# ── Apply DEPFLAGS to every vendor CFLAGS variant ────────────────────
# Bundled here (rather than inline in each := definition) so the policy
# is visible in one place and so we don't fight backslash-continuation
# parsing in multi-line vendor flag definitions.
QJS_CFLAGS       += $(DEPFLAGS)
LUA_CFLAGS       += $(DEPFLAGS)
MBEDTLS_CFLAGS   += $(DEPFLAGS)
SQLITE_CFLAGS    += $(DEPFLAGS)
LOG_CFLAGS       += $(DEPFLAGS)
SH_ARENA_CFLAGS  += $(DEPFLAGS)
SH_JSON_CFLAGS   += $(DEPFLAGS)
TWEETNACL_CFLAGS += $(DEPFLAGS)
STB_CFLAGS       += $(DEPFLAGS)

# ── WAMR (WebAssembly Micro Runtime — compute-only) ──────────────
#
# ── WASM (WAMR) ───────────────────────────────────────────────────────
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

# Native invoker: 64-bit platforms require platform-specific assembly invokers.
# The generic C invoker (invokeNative_general.c) only works on 32-bit platforms
# because it passes argv[] as uint32 values, but 64-bit wasm_runtime_invoke_native
# packs arguments into uint64 slots. Using the generic invoker on 64-bit causes
# argument mangling (pointer split across two params, shifted integer args).
# When SIMD is enabled, float register slots are 128-bit (v128), so the assembly
# invoker must use the _simd variant to match the buffer layout.
WAMR_INVOKE_OBJ := $(BUILDDIR)/wamr_invoke_native.o
# Target-compiler checks must come BEFORE host-platform (UNAME_S=Darwin),
# otherwise cross-building cosmo on macOS picks invokeNative_osx_universal.s
# (Mach-O) for a cosmo (ELF/APE) link and the linker errors with
# "missing elf symbol table". §3.1 of roadmap_next.md.
ifeq ($(notdir $(CC)),cosmocc)
  # Cosmopolitan fat binary: need arch-specific assembly invokers for both
  # x86_64 and aarch64. The generic C invoker mangles 64-bit arguments.
  # cosmocc links two architectures; we compile each with the arch-specific
  # compiler and place the aarch64 object in build/.aarch64/ per convention.
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_em64_simd.s
  WAMR_INVOKE_SRC_ARM64 := $(WAMR_IWASM)/common/arch/invokeNative_aarch64_simd.s
  WAMR_INVOKE_OBJ_ARM64 := $(BUILDDIR)/.aarch64/wamr_invoke_native.o
else ifneq ($(findstring x86_64-unknown-cosmo,$(CC)),)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_em64_simd.s
else ifneq ($(findstring aarch64-unknown-cosmo,$(CC)),)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_aarch64_simd.s
else ifeq ($(UNAME_S),Darwin)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_osx_universal.s
  WAMR_INVOKE_FLAGS := -DBH_PLATFORM_DARWIN -DWASM_ENABLE_SIMD=1
else ifeq ($(shell uname -m),x86_64)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_em64_simd.s
else ifeq ($(shell uname -m),aarch64)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_aarch64_simd.s
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

WAMR_CFLAGS += $(DEPFLAGS)

else
# WASM disabled
WAMR_OBJS :=
WAMR_INC  :=
endif

# ── TinyCC (embedded C compiler for zero-dependency hull build) ──────
#
# On by default for non-cosmo builds. Disable with HL_ENABLE_TCC=0.
# Requires vendor/tcc submodule (git submodule add -b mob ...)
#
# Build tcc: make tcc  (builds build/tcc from vendor/tcc source)

TCC_DIR         := vendor/tcc
EMBEDDED_TCC_H  := $(BUILDDIR)/embedded_tcc.h
COMPILER_OBJ    := $(BUILDDIR)/compiler.o
COMPILER_TCC_OBJ :=

ifdef COSMO
  HL_ENABLE_TCC ?= 0
else
  HL_ENABLE_TCC ?= 1
endif

# ── HL_ENABLE_TUI — terminal UI capability ─────────────────────────
#
# Off this flag drops cap/tui.c, cap/tui_input.c, cap/tui_width.c,
# the runtime bindings, and the stdlib `hull/tui` module. Width-only
# (cap_tui_width.c) is light enough to keep around for other uses,
# but for simplicity the cap_tui_* trio is gated together.
#
# Default: on. Cosmo: on (POSIX termios + ANSI; no platform deps).
HL_ENABLE_TUI ?= 1

ifeq ($(HL_ENABLE_TUI),1)
CFLAGS += -DHL_ENABLE_TUI
endif

ifeq ($(HL_ENABLE_TCC),1)
CFLAGS += -DHL_ENABLE_TCC

# Build tcc binary from vendored source
.PHONY: tcc
tcc: $(BUILDDIR)/tcc

$(BUILDDIR)/tcc: $(TCC_DIR)/tcc.c | $(BUILDDIR)
	cd $(TCC_DIR) && ./configure
	$(MAKE) -C $(TCC_DIR) tcc
	cp $(TCC_DIR)/tcc $(BUILDDIR)/tcc
	chmod +x $(BUILDDIR)/tcc

# xxd tcc binary → C header
# Depends on $(BUILDDIR)/tcc so a clean build always embeds a real tcc
# (instead of silently shipping a stub that breaks --compiler=tcc).
$(EMBEDDED_TCC_H): $(BUILDDIR)/tcc | $(BUILDDIR)
	xxd -i -n embedded_tcc $(BUILDDIR)/tcc > $@
	tcc_ver=$$($(BUILDDIR)/tcc --version 2>&1 | head -1); \
		printf 'static const char hl_tcc_version_str[] = "%s";\n' "$$tcc_ver" >> $@

# compiler_tcc.o depends on embedded_tcc.h
COMPILER_TCC_OBJ := $(BUILDDIR)/compiler_tcc.o
$(COMPILER_TCC_OBJ): $(SRCDIR)/hull/compiler_tcc.c $(EMBEDDED_TCC_H) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

endif

# compiler.c — always compiled (system backend + selection)
$(COMPILER_OBJ): $(SRCDIR)/hull/compiler.c $(INCDIR)/hull/compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# ── Embedded CA bundle (Mozilla, via curl.se) ──────────────────────
#
# Embeds vendor/cacert/cacert.pem into the binary so HTTPS works in
# environments without a system CA store (Cosmo on Windows, FROM scratch
# containers, alpine, air-gapped). Default: on. Disable with HL_EMBED_CA_BUNDLE=0.
#
# To refresh the bundle: `make fetch-ca-bundle`

HL_EMBED_CA_BUNDLE ?= 1

CACERT_DIR        := vendor/cacert
CACERT_PEM        := $(CACERT_DIR)/cacert.pem
CACERT_SHA256     := $(CACERT_DIR)/cacert.pem.sha256
EMBEDDED_CACERT_H := $(BUILDDIR)/embedded_cacert.h

ifeq ($(HL_EMBED_CA_BUNDLE),1)
CFLAGS += -DHL_EMBED_CA_BUNDLE

# xxd the bundle into a NUL-terminated C array (mbedTLS PEM parser requires NUL).
# Pre-pend a temp file with the source bundle + a trailing NUL so the embedded
# array is itself NUL-terminated when mbedtls_x509_crt_parse reads it.
#
# Also extract the "last updated" line from the PEM header and emit it as
# HL_CA_BUNDLE_DATE so `hull doctor` can display it.
$(EMBEDDED_CACERT_H): $(CACERT_PEM) | $(BUILDDIR)
	cp $(CACERT_PEM) $(BUILDDIR)/cacert.pem.tmp
	printf '\0' >> $(BUILDDIR)/cacert.pem.tmp
	xxd -i -n embedded_cacert $(BUILDDIR)/cacert.pem.tmp > $@
	@date=$$(grep 'last updated on:' $(CACERT_PEM) | head -1 | sed 's/.*last updated on: *//;s/ *$$//'); \
		printf '#define HL_CA_BUNDLE_DATE "%s"\n' "$$date" >> $@
	rm -f $(BUILDDIR)/cacert.pem.tmp
endif

.PHONY: fetch-ca-bundle
fetch-ca-bundle:
	@mkdir -p $(CACERT_DIR)
	@echo "Fetching Mozilla CA bundle from curl.se …"
	curl -fsSL https://curl.se/ca/cacert.pem -o $(CACERT_PEM)
	curl -fsSL https://curl.se/ca/cacert.pem.sha256 -o $(CACERT_SHA256)
	@echo "Verifying SHA-256 …"
	@cd $(CACERT_DIR) && (sha256sum -c cacert.pem.sha256 2>/dev/null \
	    || shasum -a 256 -c cacert.pem.sha256)
	@echo "Done — $$(grep -c '^-----BEGIN CERTIFICATE-----' $(CACERT_PEM)) certificates."

# ── Embedded pwned-password blocklist (SecLists top 10K) ─────────────
#
# Refreshes stdlib/{lua,js}/hull/web/_pwned_blocklist.{lua,js} from
# the upstream SecLists 10K most-common passwords list. Output is a
# sorted, deduped list of 8-char uppercase SHA-1 prefixes, binary-
# searched at request time by hull/web/pwned BEFORE the network
# round-trip. Keeps the air-gapped fail-open from silently letting
# every weak password through. Source license: CC-BY-3.0.

PWNED_SRC_URL := https://raw.githubusercontent.com/danielmiessler/SecLists/master/Passwords/Common-Credentials/10k-most-common.txt
PWNED_SRC_TMP := /tmp/seclists_top10k.txt

.PHONY: fetch-pwned-blocklist
fetch-pwned-blocklist:
	@echo "Fetching SecLists top 10K from danielmiessler/SecLists …"
	curl -fsSL $(PWNED_SRC_URL) -o $(PWNED_SRC_TMP)
	@bash scripts/build_pwned_blocklist.sh $(PWNED_SRC_TMP)

# ── HTMX + Pico (vendored assets for `hull init --profile htmx`) ───
#
# Pinned releases. Bumping versions: update the version + SHA-256
# variables below, run `make fetch-htmx fetch-pico`, sanity-check
# that the new bytes serve a working HTMX scaffold, then commit.
#
# These files are committed under vendor/htmx/ and vendor/pico/ at
# the pinned versions. They get embedded by the scaffold step (see
# §1.5.a-5 — `hull init --profile htmx`) when generating a new app's
# static/vendor/ directory. Apps own their copies after scaffolding;
# bumping Hull's pinned versions doesn't touch existing apps.

HTMX_DIR        := vendor/htmx
HTMX_VERSION    := v2.0.9
HTMX_MIN_SHA256 := 57d9191515339922bd1356d7b2d80b1ee3b29f1b3a2c65a078bb8b2e8fd9ae5f
HTMX_URL        := https://github.com/bigskysoftware/htmx/releases/download/$(HTMX_VERSION)/htmx.min.js
HTMX_MIN_JS     := $(HTMX_DIR)/htmx.min.js

PICO_DIR             := vendor/pico
PICO_VERSION         := v2.1.1
PICO_CLASSLESS_SHA256 := 61207a40ffc02a42d1e50143651c121beab70ed413c934c1ff84fa263ba436b0
PICO_URL             := https://raw.githubusercontent.com/picocss/pico/$(PICO_VERSION)/css/pico.classless.min.css
PICO_CLASSLESS_CSS   := $(PICO_DIR)/pico.classless.min.css

.PHONY: fetch-htmx
fetch-htmx:
	@mkdir -p $(HTMX_DIR)
	@echo "Fetching HTMX $(HTMX_VERSION) from github.com/bigskysoftware/htmx …"
	curl -fsSL $(HTMX_URL) -o $(HTMX_MIN_JS)
	@echo "Verifying SHA-256 (pinned: $(HTMX_MIN_SHA256)) …"
	@actual=$$(shasum -a 256 $(HTMX_MIN_JS) | awk '{print $$1}'); \
	if [ "$$actual" != "$(HTMX_MIN_SHA256)" ]; then \
	    echo "SHA-256 mismatch for HTMX!"; \
	    echo "  expected: $(HTMX_MIN_SHA256)"; \
	    echo "  actual:   $$actual"; \
	    rm -f $(HTMX_MIN_JS); \
	    exit 1; \
	fi
	@echo "$(HTMX_VERSION)" > $(HTMX_DIR)/VERSION
	@echo "Done — htmx.min.js ($$(wc -c < $(HTMX_MIN_JS)) bytes)."

.PHONY: fetch-pico
fetch-pico:
	@mkdir -p $(PICO_DIR)
	@echo "Fetching Pico classless $(PICO_VERSION) from github.com/picocss/pico …"
	curl -fsSL $(PICO_URL) -o $(PICO_CLASSLESS_CSS)
	@echo "Verifying SHA-256 (pinned: $(PICO_CLASSLESS_SHA256)) …"
	@actual=$$(shasum -a 256 $(PICO_CLASSLESS_CSS) | awk '{print $$1}'); \
	if [ "$$actual" != "$(PICO_CLASSLESS_SHA256)" ]; then \
	    echo "SHA-256 mismatch for Pico!"; \
	    echo "  expected: $(PICO_CLASSLESS_SHA256)"; \
	    echo "  actual:   $$actual"; \
	    rm -f $(PICO_CLASSLESS_CSS); \
	    exit 1; \
	fi
	@echo "$(PICO_VERSION)" > $(PICO_DIR)/VERSION
	@echo "Done — pico.classless.min.css ($$(wc -c < $(PICO_CLASSLESS_CSS)) bytes)."

.PHONY: fetch-htmx-pico
fetch-htmx-pico: fetch-htmx fetch-pico

# ── Unicode width data (refresh + regenerate) ──────────────────────
#
# `make fetch-unicode` downloads fresh EastAsianWidth.txt and
# UnicodeData.txt from unicode.org, verifies SHA-256 against the
# pinned checksums, then regenerates vendor/unicode/eaw.h via
# vendor/unicode/gen.lua. Checked-in artifacts cover hermetic builds;
# this target is invoked manually when upgrading Unicode versions.
#
# Pin to a specific Unicode release via HL_UNICODE_VERSION.

HL_UNICODE_VERSION ?= 16.0.0
UNICODE_BASE_URL   := https://www.unicode.org/Public/$(HL_UNICODE_VERSION)/ucd
UNICODE_EAW_TXT    := $(UNICODE_DIR)/EastAsianWidth.txt
UNICODE_UCD_TXT    := $(UNICODE_DIR)/UnicodeData.txt
UNICODE_EAW_H      := $(UNICODE_DIR)/eaw.h

.PHONY: fetch-unicode
fetch-unicode:
	@mkdir -p $(UNICODE_DIR)
	@echo "Fetching Unicode $(HL_UNICODE_VERSION) data from unicode.org …"
	curl -fsSL $(UNICODE_BASE_URL)/EastAsianWidth.txt -o $(UNICODE_EAW_TXT)
	curl -fsSL $(UNICODE_BASE_URL)/UnicodeData.txt   -o $(UNICODE_UCD_TXT)
	@echo "Recording SHA-256 …"
	@cd $(UNICODE_DIR) && (shasum -a 256 EastAsianWidth.txt > EastAsianWidth.txt.sha256 \
	    || sha256sum    EastAsianWidth.txt > EastAsianWidth.txt.sha256)
	@cd $(UNICODE_DIR) && (shasum -a 256 UnicodeData.txt   > UnicodeData.txt.sha256 \
	    || sha256sum    UnicodeData.txt   > UnicodeData.txt.sha256)
	@echo "Regenerating eaw.h via gen.lua …"
	@command -v lua >/dev/null && LUA=lua || LUA=luajit; \
	    $$LUA $(UNICODE_DIR)/gen.lua $(UNICODE_EAW_TXT) $(UNICODE_UCD_TXT) > $(UNICODE_EAW_H)
	@echo "Done — $$(grep -c '^    {' $(UNICODE_EAW_H)) ranges in $(UNICODE_EAW_H)."

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
PLEDGE_CFLAGS := -std=c11 -O2 -w -D_GNU_SOURCE -I$(PLEDGE_DIR) $(DEPFLAGS)

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

# Capability sources (always compiled, except cap/tool.c and cap/test.c
# which need runtimes / linker visibility from runtime bindings).
# Runtime-layer test bindings live in runtime/{lua,js}/mod_test.c (picked
# up via the JS_RT_SRCS / LUA_RT_SRCS globs below).
CAP_SRCS := $(filter-out $(SRCDIR)/hull/cap/tool.c $(SRCDIR)/hull/cap/test.c,$(wildcard $(SRCDIR)/hull/cap/*.c))
ifeq ($(HL_ENABLE_DB),0)
  # Drop SQLite-backed capability modules in pure-compute builds.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/db.c \
      $(SRCDIR)/hull/cap/db_sqlite.c \
      $(SRCDIR)/hull/cap/db_udf.c, \
      $(CAP_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_CLIENT),0)
  # CLIENT-only capability sources — http.fetch sync + async + SMTP send.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/http.c \
      $(SRCDIR)/hull/cap/http_async.c \
      $(SRCDIR)/hull/cap/smtp.c, \
      $(CAP_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
  # SERVER-only capability sources — body reader (request bodies) +
  # WebSocket server. cap/test.c (in-process HTTP harness) is handled
  # separately below.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/ws.c \
      $(SRCDIR)/hull/cap/body.c, \
      $(CAP_SRCS))
endif
ifeq ($(HL_ENABLE_TUI),0)
  # Drop the TUI capability when disabled. cap/tui_width.c stays
  # compiled regardless — it's a pure data-table lookup with no
  # platform deps, useful elsewhere.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/tui.c \
      $(SRCDIR)/hull/cap/tui_input.c, \
      $(CAP_SRCS))
endif
CAP_OBJS := $(patsubst $(SRCDIR)/hull/cap/%.c,$(BUILDDIR)/cap_%.o,$(CAP_SRCS))
CAP_TOOL_OBJ := $(BUILDDIR)/cap_tool.o
# cap/test.c is the in-process HTTP test harness — depends on KlRouter
# and the rest of Keel's request/response machinery. Server-only.
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
CAP_TEST_OBJ :=
else
CAP_TEST_OBJ := $(BUILDDIR)/cap_test.o
endif

# JS runtime sources
JS_RT_SRCS := $(wildcard $(SRCDIR)/hull/runtime/js/*.c)
ifeq ($(HL_ENABLE_DB),0)
  JS_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/js/mod_db.c \
      $(SRCDIR)/hull/runtime/js/worker_db.c, \
      $(JS_RT_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_CLIENT),0)
  # CLIENT-only JS bindings: http.fetch + SMTP send.
  JS_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/js/mod_http_client.c \
      $(SRCDIR)/hull/runtime/js/mod_smtp.c, \
      $(JS_RT_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
  # SERVER-only JS bindings: app.ws/app.sse/app.get/etc + their backends.
  # mod_test depends on hl_cap_test_dispatch which is in cap/test.c —
  # both drop together (cap/test is filtered above).
  JS_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/js/mod_ws_server.c \
      $(SRCDIR)/hull/runtime/js/mod_ws_client.c \
      $(SRCDIR)/hull/runtime/js/mod_http_server.c \
      $(SRCDIR)/hull/runtime/js/mod_sse.c \
      $(SRCDIR)/hull/runtime/js/mod_test.c \
      $(SRCDIR)/hull/runtime/js/routes.c \
      $(SRCDIR)/hull/runtime/js/dispatch.c \
      $(SRCDIR)/hull/runtime/js/sse.c \
      $(SRCDIR)/hull/runtime/js/ws.c \
      $(SRCDIR)/hull/runtime/js/timers.c \
      $(SRCDIR)/hull/runtime/js/bindings.c, \
      $(JS_RT_SRCS))
endif
JS_RT_OBJS := $(patsubst $(SRCDIR)/hull/runtime/js/%.c,$(BUILDDIR)/js_%.o,$(JS_RT_SRCS))

# Lua runtime sources
LUA_RT_SRCS := $(wildcard $(SRCDIR)/hull/runtime/lua/*.c)
ifeq ($(HL_ENABLE_DB),0)
  LUA_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/lua/mod_db.c \
      $(SRCDIR)/hull/runtime/lua/worker_db.c, \
      $(LUA_RT_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_CLIENT),0)
  LUA_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/lua/mod_http_client.c \
      $(SRCDIR)/hull/runtime/lua/mod_smtp.c, \
      $(LUA_RT_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
  LUA_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/lua/mod_ws_server.c \
      $(SRCDIR)/hull/runtime/lua/mod_ws_client.c \
      $(SRCDIR)/hull/runtime/lua/mod_http_server.c \
      $(SRCDIR)/hull/runtime/lua/mod_sse.c \
      $(SRCDIR)/hull/runtime/lua/mod_test.c \
      $(SRCDIR)/hull/runtime/lua/routes.c \
      $(SRCDIR)/hull/runtime/lua/dispatch.c \
      $(SRCDIR)/hull/runtime/lua/sse.c \
      $(SRCDIR)/hull/runtime/lua/ws.c \
      $(SRCDIR)/hull/runtime/lua/timers.c \
      $(SRCDIR)/hull/runtime/lua/bindings.c, \
      $(LUA_RT_SRCS))
endif
LUA_RT_OBJS := $(patsubst $(SRCDIR)/hull/runtime/lua/%.c,$(BUILDDIR)/lua_rt_%.o,$(LUA_RT_SRCS))

# Command module sources
CMD_SRCS := $(wildcard $(SRCDIR)/hull/commands/*.c)
ifeq ($(HL_ENABLE_DB),0)
  CMD_SRCS := $(filter-out $(SRCDIR)/hull/commands/migrate.c,$(CMD_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_CLIENT),0)
  # `hull update` and `hull tools install` use Keel's HTTPS client to
  # fetch releases / tool binaries.
  CMD_SRCS := $(filter-out \
      $(SRCDIR)/hull/commands/update.c \
      $(SRCDIR)/hull/commands/tools.c, \
      $(CMD_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
  # SERVER-side commands:
  #   - `hull dev` forks a serve subprocess (no server = no point).
  #   - `hull test` exercises the in-process HTTP harness (cap/test.c +
  #     test_runner.c, both KlRouter-bound).
  #   - `hull agent` / `hull mcp` are introspection tools targeting a
  #     running HTTP server (most subcommands call hl_agent_endpoint /
  #     hl_agent_request / hl_agent_test which need a live harness).
  # A future `cmd_agent_cli.c` could expose just the DB / schema / routes
  # subcommands that don't need a running server.
  CMD_SRCS := $(filter-out \
      $(SRCDIR)/hull/commands/dev.c \
      $(SRCDIR)/hull/commands/test.c \
      $(SRCDIR)/hull/commands/agent.c \
      $(SRCDIR)/hull/commands/mcp.c, \
      $(CMD_SRCS))
endif
CMD_OBJS := $(patsubst $(SRCDIR)/hull/commands/%.c,$(BUILDDIR)/cmd_%.o,$(CMD_SRCS))

# Helpers shared by every runtime cache module (arch/endian tag,
# hex encoder, lazy blob_store singleton). Built once, linked into
# every cache. Defined here (above the runtime selection) so the
# `:=` assignments below see it.
RUNTIME_CACHE_COMMON_OBJ := $(BUILDDIR)/runtime_cache_common.o

# Select which runtimes to build. `RUNTIME_CACHE_COMMON_OBJ` is added
# unconditionally — every runtime cache (Lua + JS, bytecode + template)
# links against the same shared helpers, so any build that compiles
# any runtime needs it.
ifeq ($(RUNTIME),js)
  RT_OBJS   := $(JS_RT_OBJS) $(RUNTIME_CACHE_COMMON_OBJ)
  VEND_OBJS := $(QJS_OBJS)
  CFLAGS    += -DHL_ENABLE_JS
else ifeq ($(RUNTIME),lua)
  RT_OBJS   := $(LUA_RT_OBJS) $(RUNTIME_CACHE_COMMON_OBJ)
  VEND_OBJS := $(LUA_OBJS)
  CFLAGS    += -DHL_ENABLE_LUA
else
  # default: both runtimes
  RT_OBJS   := $(JS_RT_OBJS) $(LUA_RT_OBJS) $(RUNTIME_CACHE_COMMON_OBJ)
  VEND_OBJS := $(QJS_OBJS) $(LUA_OBJS)
  CFLAGS    += -DHL_ENABLE_JS -DHL_ENABLE_LUA
endif

ALLOC_OBJ      := $(BUILDDIR)/hull_alloc.o
ASYNC_OBJ      := $(BUILDDIR)/hull_async.o
# hull_compress.c wraps kl_response_body_compress for HTTP response
# bodies (server-side). Drop when the server is off.
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
COMPRESS_OBJ   :=
else
COMPRESS_OBJ   := $(BUILDDIR)/hull_compress.o
endif
MINIZ_OBJ      := $(BUILDDIR)/miniz.o
ifeq ($(HL_ENABLE_DB),0)
WORKER_DB_OBJ  :=
else
WORKER_DB_OBJ  := $(BUILDDIR)/worker_db.o
endif
WORKER_WASM_OBJ := $(BUILDDIR)/worker_wasm.o
MANIFEST_OBJ     := $(BUILDDIR)/manifest.o $(BUILDDIR)/manifest_lua.o $(BUILDDIR)/manifest_js.o $(BUILDDIR)/manifest_extract_file.o
MODULE_REGISTRY_OBJ := $(BUILDDIR)/module_registry.o
MODULE_RESOLVER_OBJ := $(BUILDDIR)/module_resolver.o
MODULE_OBJ       := $(MODULE_REGISTRY_OBJ) $(MODULE_RESOLVER_OBJ)
SANDBOX_OBJ      := $(BUILDDIR)/sandbox.o

# Async backend implementations
#   async/keel.c — wraps Keel's KlEventCtx + KlThreadPool. Compiled
#                  whenever Keel is linked (either HTTP half on).
#   async/poll.c — freestanding poll(2) + pthread impl. Always built;
#                  selected by hl_async_backend() when neither HTTP
#                  half is compiled in.
ASYNC_BACKEND_SRCS := $(wildcard $(SRCDIR)/hull/async/*.c)
ifeq ($(HL_ENABLE_HTTP_ANY),0)
  ASYNC_BACKEND_SRCS := $(filter-out $(SRCDIR)/hull/async/keel.c,$(ASYNC_BACKEND_SRCS))
endif
ASYNC_BACKEND_OBJS := $(patsubst $(SRCDIR)/hull/async/%.c,$(BUILDDIR)/async_%.o,$(ASYNC_BACKEND_SRCS))

# Net backend implementations
#   net/keel.c — Keel-backed HlNetBackend (op_suspend / op_complete
#                pair). Server-only: the only callers are server-side
#                connection-bound request suspension. CLIENT-only or
#                pure-compute builds use the no-op stubs in async/poll.c.
NET_BACKEND_SRCS := $(wildcard $(SRCDIR)/hull/net/*.c)
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
  NET_BACKEND_SRCS :=
endif
NET_BACKEND_OBJS := $(patsubst $(SRCDIR)/hull/net/%.c,$(BUILDDIR)/net_%.o,$(NET_BACKEND_SRCS))

# Test-specific objects (single runtime — avoids pulling Lua into JS tests
# and vice versa). After roadmap item G the per-runtime extractors live in
# their own .c files that auto-#ifdef-out, so single-runtime tests just
# pull manifest.o (shared helpers) + the relevant extractor.
MANIFEST_JS_OBJ  := $(BUILDDIR)/manifest.o $(BUILDDIR)/manifest_js.o
MANIFEST_LUA_OBJ := $(BUILDDIR)/manifest.o $(BUILDDIR)/manifest_lua.o
CAP_TEST_JS_OBJ  := $(BUILDDIR)/cap_test_dispatch.o
CAP_TEST_LUA_OBJ := $(BUILDDIR)/cap_test_dispatch.o
# Note: the JS/Lua test bindings now live in {JS,LUA}_RT_OBJS
# (runtime/{lua,js}/mod_test.c); only the pure-C dispatch helper needs
# to be linked separately. (Item E: cap layer is now runtime-free.)
TOOL_OBJ       := $(BUILDDIR)/tool.o $(BUILDDIR)/tool_orchestration.o
SIG_OBJ        := $(BUILDDIR)/signature.o
RELEASE_OBJ    := $(BUILDDIR)/release.o
# release_io.o is the shared HTTPS + manifest + atomic-install
# plumbing used by `hull update` and `hull tools install`.
# Only linked when HL_ENABLE_HTTP_CLIENT is on; without an HTTPS
# client there's no remote-fetch surface, so neither command is
# compiled in either.
ifeq ($(HL_ENABLE_HTTP_CLIENT),0)
RELEASE_IO_OBJ :=
else
RELEASE_IO_OBJ := $(BUILDDIR)/release_io.o
endif
# tools_install.o is always linked — `hl_tools_lookup_path` is used by
# cap/wasm.c for wamrc resolution even on HL_ENABLE_HTTP_CLIENT=0 builds.
TOOLS_INSTALL_OBJ := $(BUILDDIR)/tools_install.o
# Platform manifest builder + signer + verifier + per-arch extractor.
# Pure functions; reuses release.c (sign/verify) + release_io.c
# (find_checksum). Always built — verify path uses it on every signed
# app, regardless of HL_ENABLE_HTTP_CLIENT.
PLATFORM_SIG_OBJ := $(BUILDDIR)/platform_sig.o
# Accessor for the embedded signed platform-sig blob. CI's
# sign-platform-manifest job emits the header it includes; local
# builds get a placeholder that signals "no embedded blob" via the
# accessor's -1 return. NOT in PLATFORM_OBJS — apps don't need the
# embedded blob (they carry their own copy in package.sig.platform);
# only the hull binary itself reads it.
EMBEDDED_PLATFORM_SIG_OBJ := $(BUILDDIR)/embedded_platform_sig.o
EMBEDDED_PLATFORM_SIG_H   := $(BUILDDIR)/embedded_platform_sig.h
# Auto-detect whether the real (CI-signed) header is present. If the
# file exists AND is non-trivial (>1 KB, since real signed blob runs
# ~hundreds of bytes per arch + sig), enable HL_EMBED_PLATFORM_SIG.
# Otherwise the placeholder accessor short-circuits.
ifneq ($(wildcard $(EMBEDDED_PLATFORM_SIG_H)),)
HL_EMBED_PLATFORM_SIG := $(shell test $$(wc -c < $(EMBEDDED_PLATFORM_SIG_H) 2>/dev/null || echo 0) -gt 1024 && echo 1 || echo 0)
else
HL_EMBED_PLATFORM_SIG := 0
endif
ifeq ($(HL_EMBED_PLATFORM_SIG),1)
CFLAGS += -DHL_EMBED_PLATFORM_SIG
endif
# test_runner.c uses KlRouter to dispatch in-process test requests —
# server-only.
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
TEST_RUNNER_OBJ :=
else
TEST_RUNNER_OBJ := $(BUILDDIR)/test_runner.o
endif
RUNTIME_FACTORY_OBJ := $(BUILDDIR)/runtime_factory.o
# (RUNTIME_CACHE_COMMON_OBJ is defined earlier — see the runtime
# selection block — because RT_OBJS references it.)
# static.c serves embedded static files via Keel response writers —
# server-only.
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
STATIC_OBJ     :=
else
STATIC_OBJ     := $(BUILDDIR)/hull_static.o
endif
BUILD_ASSET_OBJ      := $(BUILDDIR)/build_assets.o
BUILD_ASSET_STUB_OBJ := $(BUILDDIR)/build_assets_stub.o
ifeq ($(HL_ENABLE_DB),0)
MIGRATE_OBJ    :=
else
MIGRATE_OBJ    := $(BUILDDIR)/migrate.o
endif
VFS_OBJ        := $(BUILDDIR)/vfs.o
PATH_NORM_OBJ  := $(BUILDDIR)/path_normalize.o
# Low-level content-addressed blob store. Shared between cap/blob.c
# (manifest-gated app capability) and runtime infrastructure (Lua
# bytecode cache, compute AOT cache, future template cache). Apps
# never see this layer directly — they go through hl_cap_blob_*.
BLOB_STORE_OBJ := $(BUILDDIR)/blob_store.o
# Compile-time registry of every cache kind hull manages (lua-bytecode,
# compute-aot, templates, tools). Single source of truth for
# `hull cache list|prune|clear`, doctor cache reporting, and inspect
# cache disclosure.
CACHE_REGISTRY_OBJ := $(BUILDDIR)/cache_registry.o
# Resolves $HOME/.hull/cache/<kind>/ for runtime-infrastructure caches
# (Lua bytecode, compute AOT, template AST). System-wide pool shared
# across apps. Sandbox auto-allows the cache root; see docs/blob.md
# §"Runtime-infrastructure caches and the manifest line".
CACHE_DIR_OBJ  := $(BUILDDIR)/cache_dir.o
CACERT_OBJ     := $(BUILDDIR)/cacert.o
# Content-Security-Policy preset registry. Tiny (~70 LOC, no deps
# beyond <string.h>) so the test surface stays cheap. Resolves
# `app.manifest({csp = "<name>"})` to a concrete header value at
# startup; unknown names pass through as literal CSP strings.
CSP_OBJ        := $(BUILDDIR)/csp.o
SBOM_OBJ       := $(BUILDDIR)/sbom.o
APP_CONTEXT_OBJ := $(BUILDDIR)/app_context.o
AGENT_LIB_SRCS := $(wildcard $(SRCDIR)/hull/agent/*.c)
ifeq ($(HL_ENABLE_DB),0)
  AGENT_LIB_SRCS := $(filter-out $(SRCDIR)/hull/agent/db.c,$(AGENT_LIB_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
  # agent/test.c calls hl_test_runner_run + the in-process HTTP harness;
  # agent/request.c, agent/eval.c, agent/perf.c, agent/endpoint.c also
  # exercise HTTP routes. All server-only — the `hull agent` subcommands
  # that depend on a running server (test/request/eval/perf/endpoint)
  # lose their backends.
  AGENT_LIB_SRCS := $(filter-out \
      $(SRCDIR)/hull/agent/test.c \
      $(SRCDIR)/hull/agent/request.c \
      $(SRCDIR)/hull/agent/eval.c \
      $(SRCDIR)/hull/agent/perf.c \
      $(SRCDIR)/hull/agent/endpoint.c, \
      $(AGENT_LIB_SRCS))
endif
AGENT_LIB_OBJ  := $(patsubst $(SRCDIR)/hull/agent/%.c,$(BUILDDIR)/agent_%.o,$(AGENT_LIB_SRCS))
# HL_ENABLE_HTTP_SERVER=0: replace serve.c (KlServer setup + routing +
# middleware + wire_routes + the full request/response lifecycle) with
# serve_cli.c (load + app.main + exit). agent_api (in-process HTTP
# introspection) drops out too — it speaks HTTP to a running server.
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
AGENT_API_OBJ  :=
SERVE_OBJ      := $(BUILDDIR)/serve_cli.o
else
AGENT_API_OBJ  := $(BUILDDIR)/agent_api.o
SERVE_OBJ      := $(BUILDDIR)/serve.o
endif
MAIN_OBJ       := $(BUILDDIR)/main.o
ENTRY_OBJ      := $(BUILDDIR)/entry.o

# ── Stdlib embedding (xxd) ──────────────────────────────────────────
#
# Two Lua source trees feed the embedded stdlib registry:
#
#   stdlib/lua/hull/*.lua       — user-facing modules apps may
#                                 require("hull.foo"): template, jwt,
#                                 cookie, csrf, csv, email, form, i18n,
#                                 json, search, validate, plus
#                                 middleware/*.
#
#   stdlib/cli/lua/hull/*.lua   — CLI plugins invoked only by the C
#                                 dispatcher (`hull build`, `hull deploy`,
#                                 etc.) via hull_tool(); never imported
#                                 by app code. Split out per audit A-2
#                                 so stdlib/lua/hull/ honestly reflects
#                                 the user-facing surface.
#
# Both trees go through the same xxd pipeline and end up in
# hl_stdlib_entries[]. The name-strip rule below makes
# stdlib/cli/lua/hull/build.lua resolve as "hull.build" — same name
# the C dispatcher and any cross-CLI require already use, so this is
# a path move with no code change required.

STDLIB_LUA_USER_FILES := $(shell find stdlib/lua -name '*.lua' -not -path '*/tests/*' 2>/dev/null)
STDLIB_LUA_CLI_FILES  := $(shell find stdlib/cli/lua -name '*.lua' -not -path '*/tests/*' 2>/dev/null)
STDLIB_LUA_FILES      := $(STDLIB_LUA_USER_FILES) $(STDLIB_LUA_CLI_FILES)

# User-facing: stdlib/lua/hull/foo.lua → build/stdlib_lua_hull_foo.h
stdlib_hdr     = $(BUILDDIR)/$(subst /,_,$(patsubst stdlib/%.lua,stdlib_%.h,$(1)))
# CLI plugins:  stdlib/cli/lua/hull/build.lua → build/stdlib_cli_lua_hull_build.h
stdlib_cli_hdr = $(BUILDDIR)/$(subst /,_,$(patsubst stdlib/%.lua,stdlib_%.h,$(1)))

STDLIB_LUA_HDRS  := $(foreach f,$(STDLIB_LUA_USER_FILES),$(call stdlib_hdr,$(f)))
STDLIB_LUA_HDRS  += $(foreach f,$(STDLIB_LUA_CLI_FILES),$(call stdlib_cli_hdr,$(f)))

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

# ── Stdlib-shipped static assets (xxd) ────────────────────────────────
#
# Files under stdlib/static/hull/<module>/* become embedded entries
# named `static/hull/<module>/<file>`. The static middleware (src/hull/
# static.c) falls back to the platform VFS after an app-VFS miss, so
# `/static/hull/<module>/<file>` requests resolve here automatically.
# Convention: stdlib widgets ship assets under their own subdirectory;
# apps may override by writing the same path under their own static/.
# Phase 0 ships no widget assets — discovery is wired and tested but
# returns 0 entries until §1.5.g-1 lands.

STDLIB_STATIC_FILES := $(shell find stdlib/static -type f \
    -not -name '.gitkeep' 2>/dev/null)

# Path flatten: stdlib/static/hull/htmx/toast/toast.css ->
#               build/stdlib_static_hull_htmx_toast_toast.css.h
stdlib_static_hdr = $(BUILDDIR)/stdlib_static_$(subst /,_,$(patsubst stdlib/static/%,%.h,$(1)))
STDLIB_STATIC_HDRS := $(foreach f,$(STDLIB_STATIC_FILES),$(call stdlib_static_hdr,$(f)))

define STDLIB_STATIC_RULE
$(call stdlib_static_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(STDLIB_STATIC_FILES),$(eval $(call STDLIB_STATIC_RULE,$(f))))

STDLIB_STATIC_XXD_HDRS := $(STDLIB_STATIC_HDRS)

# ── Stdlib-shipped template partials (xxd) ────────────────────────────
#
# Files under stdlib/templates/hull/<module>/*.html become embedded
# entries named `templates/hull/<module>/<file>`. The template engine
# (stdlib/lua/hull/template.lua + JS sibling) falls back to the
# platform VFS after an app-VFS miss; app-side templates at the same
# path win. Phase 0 ships no widget templates.

STDLIB_TPL_FILES := $(shell find stdlib/templates -name '*.html' \
    -not -name '.gitkeep' 2>/dev/null)

# Path flatten: stdlib/templates/hull/htmx/toast/toast.html ->
#               build/stdlib_tpl_hull_htmx_toast_toast.h
stdlib_tpl_hdr = $(BUILDDIR)/stdlib_tpl_$(subst /,_,$(patsubst stdlib/templates/%.html,%.h,$(1)))
STDLIB_TPL_HDRS := $(foreach f,$(STDLIB_TPL_FILES),$(call stdlib_tpl_hdr,$(f)))

define STDLIB_TPL_RULE
$(call stdlib_tpl_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(STDLIB_TPL_FILES),$(eval $(call STDLIB_TPL_RULE,$(f))))

STDLIB_TPL_XXD_HDRS := $(STDLIB_TPL_HDRS)

# ── Unified stdlib registry (.c compiled once, linked by both runtimes) ──
#
# Merges Lua (dot names), JS (colon names), and context docs into
# a single hl_stdlib_entries[].
# Runtimes filter at load time: strchr(name, ':') → JS, else Lua.

STDLIB_REGISTRY_C := $(BUILDDIR)/stdlib_registry.c
STDLIB_REGISTRY_O := $(BUILDDIR)/stdlib_registry.o

$(STDLIB_REGISTRY_C): $(STDLIB_LUA_XXD_HDRS) $(STDLIB_JS_XXD_HDRS) $(CONTEXT_XXD_HDRS) $(STDLIB_STATIC_XXD_HDRS) $(STDLIB_TPL_XXD_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated unified stdlib registry — do not edit */" > $@
	@for hdr in $(STDLIB_LUA_XXD_HDRS) $(STDLIB_JS_XXD_HDRS) $(CONTEXT_XXD_HDRS) $(STDLIB_STATIC_XXD_HDRS) $(STDLIB_TPL_XXD_HDRS); do \
		echo "#include \"$$(basename $$hdr)\""; \
	done >> $@
	@echo "" >> $@
	@echo "#include \"hull/entry.h\"" >> $@
	@echo "const HlEntry hl_stdlib_entries[] = {" >> $@
	@( for f in $(STDLIB_LUA_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.\-]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/lua/||; s|^stdlib/cli/lua/||; s|\.lua$$||; s|/|.|g'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(STDLIB_JS_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.\-]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/js/||; s|\.js$$||; s|/|:|g'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(CONTEXT_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.\-]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/context/||; s|\.md$$||'); \
		echo "context:$$modname	    { \"context:$$modname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(STDLIB_STATIC_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.\-]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/||'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done; \
	for f in $(STDLIB_TPL_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.\-]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/||'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
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
APP_SHADER_FILES := $(shell find $(APP_DIR)/shaders -name '*.wgsl' 2>/dev/null)

# xxd header paths per file type
app_lua_hdr = $(BUILDDIR)/app_lua_$(subst /,_,$(patsubst $(APP_DIR)/%.lua,%.h,$(1)))
app_js_hdr = $(BUILDDIR)/app_js_$(subst /,_,$(patsubst $(APP_DIR)/%.js,%.h,$(1)))
app_json_hdr = $(BUILDDIR)/app_json_$(subst /,_,$(patsubst $(APP_DIR)/%.json,%.h,$(1)))
app_tpl_hdr = $(BUILDDIR)/app_tpl_$(subst /,_,$(patsubst $(APP_DIR)/templates/%.html,%.h,$(1)))
app_static_hdr = $(BUILDDIR)/app_static_$(subst /,_,$(patsubst $(APP_DIR)/static/%,%.h,$(1)))
app_migration_hdr = $(BUILDDIR)/app_mig_$(subst /,_,$(patsubst $(APP_DIR)/migrations/%,%.h,$(1)))
app_compute_hdr = $(BUILDDIR)/app_compute_$(subst /,_,$(patsubst $(APP_DIR)/compute/%,%.h,$(1)))
app_shader_hdr = $(BUILDDIR)/app_shader_$(subst /,_,$(patsubst $(APP_DIR)/shaders/%,%.h,$(1)))

APP_LUA_HDRS := $(foreach f,$(APP_LUA_FILES),$(call app_lua_hdr,$(f)))
APP_JS_HDRS := $(foreach f,$(APP_JS_FILES),$(call app_js_hdr,$(f)))
APP_JSON_HDRS := $(foreach f,$(APP_JSON_FILES),$(call app_json_hdr,$(f)))
APP_TPL_HDRS := $(foreach f,$(APP_TPL_FILES),$(call app_tpl_hdr,$(f)))
APP_STATIC_HDRS := $(foreach f,$(APP_STATIC_FILES),$(call app_static_hdr,$(f)))
APP_MIGRATION_HDRS := $(foreach f,$(APP_MIGRATION_FILES),$(call app_migration_hdr,$(f)))
APP_COMPUTE_HDRS := $(foreach f,$(APP_COMPUTE_FILES),$(call app_compute_hdr,$(f)))
APP_SHADER_HDRS := $(foreach f,$(APP_SHADER_FILES),$(call app_shader_hdr,$(f)))

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

define APP_SHADER_RULE
$(call app_shader_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@
endef
$(foreach f,$(APP_SHADER_FILES),$(eval $(call APP_SHADER_RULE,$(f))))

APP_ALL_XXD_HDRS := $(APP_LUA_HDRS) $(APP_JS_HDRS) $(APP_JSON_HDRS) $(APP_TPL_HDRS) $(APP_STATIC_HDRS) $(APP_MIGRATION_HDRS) $(APP_COMPUTE_HDRS) $(APP_SHADER_HDRS)

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
	done; \
	for f in $(APP_SHADER_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.]/_/g'); \
		shadername=$$(echo "$$f" | sed 's|^$(APP_DIR)/||'); \
		echo "$$shadername	    { \"$$shadername\", $${varname}, sizeof($${varname}) },"; \
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

INCLUDES := -I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(KEEL_INC) -I$(KEEL_DIR)/vendor/llhttp -I$(MBEDTLS_DIR)/include -I$(SQLITE_DIR) -I$(LOG_DIR) -I$(SH_ARENA_DIR) -I$(SH_JSON_DIR) -I$(TWEETNACL_DIR) -I$(STB_DIR) -I$(VENDDIR) -I$(BUILDDIR) $(WAMR_INC)

# ── Build-flag fingerprint (force-rebuild on flag change) ───────────
#
# Make tracks file mtimes; it doesn't notice when `-D` defines
# change between invocations. Without this, switching between e.g.
# `make` and `make HL_ENABLE_HTTP=0` reuses .o files compiled with
# the wrong defines — manifests as duplicate-symbol link errors
# (poll.c's stubs collide with net/keel.c's real impls), wrong
# code paths active, or stale conditional logic.
#
# Mechanism: at parse time, compute a fingerprint string of every
# flag that flows into CFLAGS. If it differs from the previous run,
# delete every Hull-owned .o (and the binaries that link them) so
# make naturally rebuilds them from source with the new flags.
# Vendor .o files (mbedTLS, WAMR, QuickJS, Lua, SQLite) are kept —
# they don't see Hull's flags, so reusing them saves real wall time.
#
# We considered the obvious "pattern-rule prereq" approach
# (`$(BUILDDIR)/cap_%.o: $(BUILD_CONFIG_FILE)`), but pattern rules
# without a recipe *cancel* the matching pattern rule (GNU Make
# manual, "Canceling Rules"). Removing stale objects is simpler and
# doesn't require touching every recipe.
BUILD_FINGERPRINT := \
  HTTP_SERVER=$(HL_ENABLE_HTTP_SERVER)|\
  HTTP_CLIENT=$(HL_ENABLE_HTTP_CLIENT)|\
  DB=$(HL_ENABLE_DB)|\
  WASM=$(HL_ENABLE_WASM)|\
  GPU=$(HL_ENABLE_GPU)|\
  TCC=$(HL_ENABLE_TCC)|\
  CA=$(HL_EMBED_CA_BUNDLE)|\
  JS=$(HL_ENABLE_JS)|\
  LUA=$(HL_ENABLE_LUA)|\
  RUNTIME=$(RUNTIME)|\
  CC=$(CC)

BUILD_CONFIG_FILE := $(BUILDDIR)/.build-config

# Parse-time: ensure builddir exists, then compare fingerprint. On
# mismatch, drop every Hull .o (cap_*, cmd_*, js_*, lua_rt_*,
# agent_*, async_*, net_*, plus the small set of unprefixed Hull
# objects under build/) and the binaries that link them. Vendor
# objects, WAMR, mbedTLS, QuickJS, Lua, SQLite stay put.
#
# libhull_platform.a is conditionally included: when
# TRUST_PLATFORM_LIB=1, the .a is a pre-downloaded artifact (CI
# release build) that we MUST NOT delete — there's no rule to
# rebuild it, deletion makes the build fail with "TRUST_PLATFORM_LIB=1
# but .a is missing." The fingerprint check still runs and clears
# Hull .o files; .a is just excluded from that purge.
ifeq ($(TRUST_PLATFORM_LIB),1)
PLATFORM_LIB_PURGE :=
else
PLATFORM_LIB_PURGE := $(BUILDDIR)/libhull_platform.a
endif

$(shell mkdir -p $(BUILDDIR))
$(shell test "$$(cat $(BUILD_CONFIG_FILE) 2>/dev/null)" = "$(BUILD_FINGERPRINT)" || { \
    rm -f $(BUILDDIR)/cap_*.o $(BUILDDIR)/cmd_*.o $(BUILDDIR)/js_*.o $(BUILDDIR)/lua_rt_*.o \
          $(BUILDDIR)/agent_*.o $(BUILDDIR)/async_*.o $(BUILDDIR)/net_*.o \
          $(BUILDDIR)/main.o $(BUILDDIR)/serve.o $(BUILDDIR)/serve_cli.o $(BUILDDIR)/entry.o \
          $(BUILDDIR)/manifest.o $(BUILDDIR)/manifest_lua.o $(BUILDDIR)/manifest_js.o \
          $(BUILDDIR)/module_registry.o $(BUILDDIR)/module_resolver.o \
          $(BUILDDIR)/sandbox.o $(BUILDDIR)/signature.o $(BUILDDIR)/release.o $(BUILDDIR)/release_io.o $(BUILDDIR)/tools_install.o $(BUILDDIR)/platform_sig.o \
          $(BUILDDIR)/test_runner.o $(BUILDDIR)/runtime_factory.o $(BUILDDIR)/hull_static.o \
          $(BUILDDIR)/migrate.o $(BUILDDIR)/vfs.o $(BUILDDIR)/cacert.o \
          $(BUILDDIR)/app_context.o $(BUILDDIR)/tool.o $(BUILDDIR)/build_assets.o \
          $(BUILDDIR)/compiler.o $(BUILDDIR)/compiler_tcc.o \
          $(BUILDDIR)/hull_alloc.o $(BUILDDIR)/hull_async.o $(BUILDDIR)/hull_compress.o \
          $(BUILDDIR)/worker_db.o $(BUILDDIR)/worker_wasm.o $(BUILDDIR)/worker_gpu.o \
          $(BUILDDIR)/stdlib_registry.o $(BUILDDIR)/app_entries_default.o \
          $(BUILDDIR)/hull $(PLATFORM_LIB_PURGE) \
          $(BUILDDIR)/test_* 2>/dev/null; \
    printf '%s\n' '$(BUILD_FINGERPRINT)' > $(BUILD_CONFIG_FILE); \
})

# ── Targets ─────────────────────────────────────────────────────────

.PHONY: all clean test debug msan e2e e2e-build e2e-http e2e-sandbox e2e-examples e2e-cli e2e-migrate e2e-templates e2e-agent e2e-context e2e-mcp e2e-agent-api e2e-compute e2e-compute-dev e2e-aot-cache e2e-cache e2e-cache-concurrent e2e-cache-cosmo e2e-tcc e2e-install e2e-ca-bundle e2e-update e2e-tools e2e-multipart e2e-attachment e2e-blob e2e-hypermedia-photos-upload e2e-jwt-asym hull-test-examples self-build check analyze cppcheck bench bench-template bench-wasm bench-gpu bench-bytecode-cache wamrc coverage lint-lua lint-js lint platform platform-cosmo

all: $(BUILDDIR)/hull

# Platform static library — everything except entry.o and build_assets.o
# Used by `hull build` to produce standalone app binaries.
# Exports hull_main() (subcommand dispatch + server logic).
# EMBEDDED_PLATFORM_SIG_OBJ is included in PLATFORM_OBJS because
# mod_tool.c (in $(RT_OBJS)) references the hl_embedded_platform_sig
# accessor. Apps linking libhull_platform.a need the symbol resolved
# at link time even though tool.platform_sig_get is only ever called
# in tool mode (hull build, hull verify), never from app runtime.
# Cost: ~hundreds of bytes per app (the embedded manifest+sig, dead
# weight at app runtime). Trade we accept for a clean symbol graph.
PLATFORM_OBJS := $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(MANIFEST_OBJ) $(MODULE_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(SANDBOX_OBJ) $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(CSP_OBJ) $(SBOM_OBJ) $(APP_CONTEXT_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(MAIN_OBJ) $(SERVE_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_STUB_OBJ) $(STDLIB_REGISTRY_O) $(WAMR_OBJS) $(VEND_OBJS) $(MBEDTLS_OBJS) \
	$(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS) \
	$(COMPILER_OBJ) $(COMPILER_TCC_OBJ)

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

# When TRUST_PLATFORM_LIB=1, treat $(PLATFORM_LIB) as a pre-built
# leaf — make doesn't re-link it from source prereqs. This is the
# release-time path: CI downloads the .a artifact that
# sign-platform-manifest hashed and we MUST embed those exact bytes
# (touch+mtime tricks aren't reliable enough — they didn't survive
# downstream make logic across multiple attempts). TRUST_PLATFORM_LIB
# bypasses the rebuild rule entirely; the .a must already exist on
# disk or the build fails with a clear error.
#
# Local devs and CI's normal `make` invocations leave it unset and
# get the usual rebuild-from-source behavior.
ifeq ($(TRUST_PLATFORM_LIB),1)
$(PLATFORM_LIB): | $(BUILDDIR)
	@test -f $@ || (echo "ERROR: TRUST_PLATFORM_LIB=1 but $@ is missing"; exit 1)
	@echo "$@: trusting pre-built artifact (TRUST_PLATFORM_LIB=1)"
else
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
endif

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
$(BUILDDIR)/hull: $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(MANIFEST_OBJ) $(MODULE_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(SANDBOX_OBJ) $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(CSP_OBJ) $(SBOM_OBJ) $(APP_CONTEXT_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_OBJ) $(COMPILER_OBJ) $(COMPILER_TCC_OBJ) $(MAIN_OBJ) $(SERVE_OBJ) $(ENTRY_OBJ) $(APP_EXTRA_OBJS) $(STDLIB_REGISTRY_O) $(WAMR_OBJS) $(VEND_OBJS) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS) $(KEEL_LIB)
	$(CC) $(LDFLAGS) -o $@ $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(MANIFEST_OBJ) $(MODULE_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(SANDBOX_OBJ) $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(CSP_OBJ) $(SBOM_OBJ) $(APP_CONTEXT_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_OBJ) $(COMPILER_OBJ) $(COMPILER_TCC_OBJ) $(MAIN_OBJ) $(SERVE_OBJ) $(ENTRY_OBJ) $(APP_EXTRA_OBJS) $(STDLIB_REGISTRY_O) $(WAMR_OBJS) $(VEND_OBJS) $(MBEDTLS_OBJS) \
		$(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS) $(KEEL_LIB) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Capability sources
$(BUILDDIR)/cap_%.o: $(SRCDIR)/hull/cap/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Command module sources
$(BUILDDIR)/cmd_%.o: $(SRCDIR)/hull/commands/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Async backend implementations (Phase 3d-2). Future net/ + http_client/
# subdirs will get their own pattern rules alongside this one.
$(BUILDDIR)/async_%.o: $(SRCDIR)/hull/async/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Net backend implementations (Phase 3d-2 deferred slice)
$(BUILDDIR)/net_%.o: $(SRCDIR)/hull/net/%.c | $(BUILDDIR)
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
	$(CC) -std=c11 -O2 -I$(MINIZ_DIR) -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -w $(DEPFLAGS) -c -o $@ $<

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

# Manifest — split into shared helpers + per-runtime extractors (item G).
# Each per-runtime .c compiles to an empty TU when its runtime is disabled,
# so no special -D filtering is needed for the test-binary single-runtime
# variants — those just pull in the relevant {manifest_lua.o, manifest_js.o}
# alongside manifest.o.
$(BUILDDIR)/manifest.o: $(SRCDIR)/hull/manifest.c $(SRCDIR)/hull/manifest_internal.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/manifest_lua.o: $(SRCDIR)/hull/manifest_lua.c $(SRCDIR)/hull/manifest_internal.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/manifest_js.o: $(SRCDIR)/hull/manifest_js.c $(SRCDIR)/hull/manifest_internal.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# manifest_extract_file.o — runtime-neutral helper that spins up a
# transient HlJS to read app.manifest({...}) from a .js entry point.
# Lives outside the manifest_lua/manifest_js split because it ties the
# JS extractor to a file-on-disk + transient-runtime workflow, not the
# pre-existing "runtime is already running" extractor flow.
$(BUILDDIR)/manifest_extract_file.o: $(SRCDIR)/hull/manifest_extract_file.c $(INCDIR)/hull/manifest_extract_file.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Module registry — canonical sorted table of first-party modules
$(MODULE_REGISTRY_OBJ): $(SRCDIR)/hull/module_registry.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Module resolver — validates manifest.modules into a frozen set
$(MODULE_RESOLVER_OBJ): $(SRCDIR)/hull/module_resolver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# cap/test.c (shared dispatch — no runtime deps, used by both runtimes)
$(BUILDDIR)/cap_test_dispatch.o: $(SRCDIR)/hull/cap/test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Sandbox (pledge/unveil enforcement)
$(SANDBOX_OBJ): $(SRCDIR)/hull/sandbox.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Signature verification
$(SIG_OBJ): $(SRCDIR)/hull/signature.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Release artifact signing / verification
$(RELEASE_OBJ): $(SRCDIR)/hull/release.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Shared HTTPS/manifest/atomic-install helpers (hull update + hull tools install)
ifneq ($(HL_ENABLE_HTTP_CLIENT),0)
$(BUILDDIR)/release_io.o: $(SRCDIR)/hull/release_io.c $(INCDIR)/hull/release_io.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
endif

# Tool registry + on-disk install / lookup helpers
# (TOOLS_INSTALL_OBJ var defined earlier so PLATFORM_OBJS sees it.)
$(TOOLS_INSTALL_OBJ): $(SRCDIR)/hull/tools_install.c $(INCDIR)/hull/tools_install.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Platform manifest builder + signer + verifier + per-arch extractor.
# Var defined earlier (near TOOLS_INSTALL_OBJ) so PLATFORM_OBJS sees it.
$(PLATFORM_SIG_OBJ): $(SRCDIR)/hull/platform_sig.c $(INCDIR)/hull/platform_sig.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Embedded platform-sig accessor. CI's sign-platform-manifest job
# generates the real header before this rule runs; locally we generate
# a placeholder so the build doesn't fail. The placeholder defines
# zero-length arrays + matching length vars; the accessor's
# `len == 0` check (via the absent HL_EMBED_PLATFORM_SIG macro) is
# what actually short-circuits — the symbols just have to compile.
#
# Note the lack of a SOURCE prereq — make never RErefreshes the
# placeholder. Once generated, the file stays. CI overwrites it
# wholesale BEFORE invoking make, so the placeholder is bypassed
# in release builds.
$(EMBEDDED_PLATFORM_SIG_H): | $(BUILDDIR)
	@if [ -f $@ ]; then \
	    echo "embedded_platform_sig: keeping existing $@ ($$(wc -c < $@) bytes)"; \
	else \
	    echo "embedded_platform_sig: generating placeholder $@"; \
	    printf '%s\n' \
	        '/* Auto-generated placeholder. CI overwrites this with the' \
	        ' * signed manifest+sig from sign-platform-manifest. The' \
	        ' * symbols exist so embedded_platform_sig.c compiles; the' \
	        ' * absent HL_EMBED_PLATFORM_SIG macro makes the accessor' \
	        ' * return -1 (no embedded blob present). */' \
	        'unsigned char hl_embedded_platform_sig_manifest[1] = { 0 };' \
	        'unsigned int  hl_embedded_platform_sig_manifest_len = 0;' \
	        'unsigned char hl_embedded_platform_sig_signature[1] = { 0 };' \
	        'unsigned int  hl_embedded_platform_sig_signature_len = 0;' \
	        > $@; \
	fi

$(EMBEDDED_PLATFORM_SIG_OBJ): $(SRCDIR)/hull/embedded_platform_sig.c $(INCDIR)/hull/embedded_platform_sig.h $(EMBEDDED_PLATFORM_SIG_H) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Shared test runner (commands/test.c + agent_lib::test)
$(TEST_RUNNER_OBJ): $(SRCDIR)/hull/test_runner.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Runtime factory registry (table-driven runtime selection — item K)
$(RUNTIME_FACTORY_OBJ): $(SRCDIR)/hull/runtime/factory.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Shared helpers for every runtime cache (Lua + JS, bytecode + template)
$(RUNTIME_CACHE_COMMON_OBJ): $(SRCDIR)/hull/runtime/cache_common.c | $(BUILDDIR)
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

# Shared path-normalize helper (used by both runtimes' module loaders)
$(PATH_NORM_OBJ): $(SRCDIR)/hull/path_normalize.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# $HOME/.hull/cache/ resolver shared by every runtime cache consumer.
$(CACHE_DIR_OBJ): $(SRCDIR)/hull/cache_dir.c $(INCDIR)/hull/cache_dir.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Low-level CAS shared by cap/blob.c and the runtime caches.
$(BLOB_STORE_OBJ): $(SRCDIR)/hull/blob_store.c $(INCDIR)/hull/blob_store.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Cache kind registry — used by `hull cache list|prune|clear`.
$(CACHE_REGISTRY_OBJ): $(SRCDIR)/hull/cache_registry.c $(INCDIR)/hull/cache_registry.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# App context (shared init for agent, test, MCP)
$(APP_CONTEXT_OBJ): $(SRCDIR)/hull/app_context.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Agent library (shared by CLI, MCP, HTTP endpoints) — one .o per
# operation, plus agent_helpers.o for write_error/open_app_db.
# Source files live under src/hull/agent/ since roadmap item I step 3.
$(BUILDDIR)/agent_%.o: $(SRCDIR)/hull/agent/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Agent API (diagnostic HTTP endpoints)
$(AGENT_API_OBJ): $(SRCDIR)/hull/agent_api.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Tool mode (keygen, build, verify, etc.)
$(BUILDDIR)/tool.o: $(SRCDIR)/hull/tool.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Tool orchestration — cross-layer bindings spliced onto the `tool`
# global after the runtime/lua thin-binding layer installs the base
# table. Lives at src/hull/ (not runtime/lua/) so commands/, dev_state,
# agent_lib, migrate, and module_* aren't pulled into runtime/ headers.
$(BUILDDIR)/tool_orchestration.o: $(SRCDIR)/hull/tool_orchestration.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Build assets (embedded platform lib — stub unless HL_BUILD_EMBEDDED=1)
# When TCC is enabled, also depends on embedded_tcc.h (generated before compilation)
ifeq ($(HL_ENABLE_TCC),1)
$(BUILD_ASSET_OBJ): $(SRCDIR)/hull/build_assets.c $(EMBEDDED_TCC_H) | $(BUILDDIR)
else
$(BUILD_ASSET_OBJ): $(SRCDIR)/hull/build_assets.c | $(BUILDDIR)
endif
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Build assets stub (no-op stubs for platform archive — satisfies cap_tool.o refs)
$(BUILD_ASSET_STUB_OBJ): $(SRCDIR)/hull/build_assets_stub.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# SBOM (Software Bill of Materials — self-describing vendored-deps table).
# Read-only data exporter. Orthogonal to the rest of the runtime:
# depends only on cacert.h (for embedded-blob SHA-256) and mbedTLS.
$(BUILDDIR)/sbom.o: $(SRCDIR)/hull/sbom.c $(INCDIR)/hull/sbom.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Main (hull_main dispatcher — small; no Keel dependency)
$(BUILDDIR)/main.o: $(SRCDIR)/hull/main.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Serve (full app lifecycle — orchestrates Keel server + runtime)
$(BUILDDIR)/serve.o: $(SRCDIR)/hull/serve.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Serve-cli (CLI counterpart, used when HL_ENABLE_HTTP_SERVER=0)
$(BUILDDIR)/serve_cli.o: $(SRCDIR)/hull/serve_cli.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Entry (thin main → hull_main trampoline — NOT in platform .a)
$(ENTRY_OBJ): $(SRCDIR)/hull/entry.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# CA bundle accessor (always compiled — empty when HL_EMBED_CA_BUNDLE=0)
ifeq ($(HL_EMBED_CA_BUNDLE),1)
$(CACERT_OBJ): $(SRCDIR)/hull/cacert.c $(INCDIR)/hull/cacert.h $(EMBEDDED_CACERT_H) | $(BUILDDIR)
else
$(CACERT_OBJ): $(SRCDIR)/hull/cacert.c $(INCDIR)/hull/cacert.h | $(BUILDDIR)
endif
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Content-Security-Policy preset registry.
$(CSP_OBJ): $(SRCDIR)/hull/csp.c $(INCDIR)/hull/csp.h | $(BUILDDIR)
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
ifeq ($(notdir $(CC)),cosmocc)
	x86_64-unknown-cosmo-cc $(WAMR_CFLAGS) $(WAMR_INVOKE_FLAGS) -c -o $@ $<
	@mkdir -p $(BUILDDIR)/.aarch64
	aarch64-unknown-cosmo-cc $(WAMR_CFLAGS) $(WAMR_INVOKE_FLAGS) -c -o $(WAMR_INVOKE_OBJ_ARM64) $(WAMR_INVOKE_SRC_ARM64)
else
	$(CC) $(WAMR_CFLAGS) $(WAMR_INVOKE_FLAGS) -c -o $@ $<
endif
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

# Drop DB-dependent tests in pure-compute builds.
# test_js.c and test_lua.c exercise the full orchestration surface
# (including db.*) — they reference hl_db_backend_sqlite directly.
# Pure-compute builds skip them; the runtime sandbox is still covered
# by the smaller runtime-isolated tests.
ifeq ($(HL_ENABLE_DB),0)
  TEST_SRCS := $(filter-out \
      %/test_db.c %/test_db_backend.c \
      %/test_js.c %/test_lua.c, \
      $(TEST_SRCS))
endif

# Flatten test paths to build/ binaries: tests/hull/cap/test_body.c → build/test_body
TEST_BINS := $(addprefix $(BUILDDIR)/,$(notdir $(basename $(TEST_SRCS))))

# Test objects need hull capability sources but NOT main.o or runtime objects
TEST_CAP_OBJS := $(CAP_OBJS)

# Shared link deps for all tests
TEST_COMMON_DEPS := $(TEST_CAP_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(KEEL_LIB)
TEST_COMMON_LIBS := $(TEST_CAP_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) $(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread
# forkpty(3) is in libutil on glibc/musl Linux (used by
# tests/hull/cap/test_tui_lifecycle.c). macOS / BSD ship it inside
# libSystem so no extra flag is needed. Cosmopolitan does not provide
# libutil at all — gating on !COSMO keeps the cosmocc CI green; the
# TUI lifecycle test on cosmo already short-circuits via the
# HL_HAVE_FORKPTY=0 path.
ifeq ($(UNAME_S),Linux)
ifndef COSMO
  TEST_COMMON_LIBS += -lutil
endif
endif

# Capability tests (tests/hull/cap/)
$(BUILDDIR)/test_%: $(TESTDIR)/hull/cap/test_%.c $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_COMMON_LIBS)

# Top-level tests (tests/hull/)
$(BUILDDIR)/test_parse_size: $(TESTDIR)/hull/test_parse_size.c $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_COMMON_LIBS)

# CSP preset registry — tiny, no deps beyond <string.h>.
$(BUILDDIR)/test_csp: $(TESTDIR)/hull/test_csp.c $(CSP_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(CSP_OBJ)

# JS runtime test — needs QuickJS + JS runtime objects + manifest (JS-only to avoid Lua link deps)
$(BUILDDIR)/test_js: $(TESTDIR)/hull/runtime/js/test_js.c $(TEST_COMMON_DEPS) $(MANIFEST_JS_OBJ) $(MODULE_OBJ) $(CAP_TEST_JS_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(RUNTIME_CACHE_COMMON_OBJ) $(JS_RT_OBJS) $(QJS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(CAP_TEST_JS_OBJ) $(JS_RT_OBJS) $(MANIFEST_JS_OBJ) $(MODULE_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(RUNTIME_CACHE_COMMON_OBJ) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(WAMR_OBJS) $(QJS_OBJS) \
		$(KEEL_LIB) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Lua runtime test — needs Lua + Lua runtime objects + manifest (Lua-only) + cap_tool + build_assets
# COMPILER_TCC_OBJ is empty when HL_ENABLE_TCC=0 (e.g. cosmocc builds),
# so it expands to nothing in both the prereq and link lines.
$(BUILDDIR)/test_lua: $(TESTDIR)/hull/runtime/lua/test_lua.c $(TEST_COMMON_DEPS) $(CAP_TOOL_OBJ) $(CAP_TEST_LUA_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cmd_doctor.o $(BUILDDIR)/cmd_dev.o $(BUILDDIR)/compiler.o $(COMPILER_TCC_OBJ) $(BUILDDIR)/tool.o $(BUILDDIR)/tool_orchestration.o $(BUILDDIR)/sandbox.o $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(APP_CONTEXT_OBJ) $(MIGRATE_OBJ) $(MANIFEST_OBJ) $(MODULE_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(RUNTIME_CACHE_COMMON_OBJ) $(LUA_RT_OBJS) $(JS_RT_OBJS) $(LUA_OBJS) $(QJS_OBJS) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(TEST_RUNNER_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(PLEDGE_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_LUA_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cmd_doctor.o $(BUILDDIR)/cmd_dev.o $(BUILDDIR)/compiler.o $(COMPILER_TCC_OBJ) $(BUILDDIR)/tool.o $(BUILDDIR)/tool_orchestration.o $(BUILDDIR)/sandbox.o $(BUILDDIR)/cacert.o $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(APP_CONTEXT_OBJ) $(MIGRATE_OBJ) $(LUA_RT_OBJS) $(JS_RT_OBJS) $(MANIFEST_OBJ) $(MODULE_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(RUNTIME_CACHE_COMMON_OBJ) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(TEST_RUNNER_OBJ) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(WAMR_OBJS) $(LUA_OBJS) $(QJS_OBJS) \
		$(KEEL_LIB) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(WGPU_LIB) $(WGPU_FRAMEWORKS) $(PLEDGE_OBJS) -lm -lpthread

# Tool hardening test — cap/tool.c compiled without runtime flags (self-contained C functions)
CAP_TOOL_NONE_OBJ := $(BUILDDIR)/cap_tool_none.o
$(CAP_TOOL_NONE_OBJ): $(SRCDIR)/hull/cap/tool.c | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/test_tool: $(TESTDIR)/hull/cap/test_tool.c $(CAP_TOOL_NONE_OBJ) $(COMPILER_OBJ) $(COMPILER_TCC_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ) | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(CAP_TOOL_NONE_OBJ) $(COMPILER_OBJ) $(COMPILER_TCC_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ)

# Compiler vtable tests
COMPILER_TEST_DEPS := $(TEST_CAP_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) $(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ)

$(BUILDDIR)/test_compiler: $(TESTDIR)/hull/compiler/test_compiler.c $(COMPILER_OBJ) $(COMPILER_TCC_OBJ) $(CAP_TOOL_NONE_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ) | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -I$(VENDDIR) -o $@ \
		$(TESTDIR)/hull/compiler/test_compiler.c \
		$(COMPILER_OBJ) $(COMPILER_TCC_OBJ) \
		$(CAP_TOOL_NONE_OBJ) $(BUILD_ASSET_OBJ) \
		$(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ) -lm

# Command dispatcher test — needs full command set (symbol resolution for command table)
$(BUILDDIR)/test_dispatch: $(TESTDIR)/hull/commands/test_dispatch.c $(CMD_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(TOOL_OBJ) $(SANDBOX_OBJ) $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(CSP_OBJ) $(SBOM_OBJ) $(APP_CONTEXT_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(TEST_COMMON_DEPS) $(RT_OBJS) $(VEND_OBJS) $(MBEDTLS_OBJS) $(MANIFEST_OBJ) $(MODULE_OBJ) $(BUILD_ASSET_OBJ) $(COMPILER_OBJ) $(COMPILER_TCC_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(PLEDGE_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(CMD_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(TOOL_OBJ) $(SANDBOX_OBJ) $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(CACHE_DIR_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(CSP_OBJ) $(SBOM_OBJ) $(APP_CONTEXT_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) \
		$(TEST_CAP_OBJS) $(RT_OBJS) $(MANIFEST_OBJ) $(MODULE_OBJ) $(BUILD_ASSET_OBJ) $(COMPILER_OBJ) $(COMPILER_TCC_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(WAMR_OBJS) $(VEND_OBJS) \
		$(KEEL_LIB) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Signature verification test — needs crypto + app_entries_default + vfs.
# Override HL_PLATFORM_PUBKEY_HEX to the all-zeros placeholder for this
# test only: the fixture's create_test_package_sig generates a fresh
# local platform keypair on every run, so the production (gethull.dev)
# pinning check at signature.c §5 would always reject it. Both §5
# (per-app platform layer) and §5b (v0.1.3 gethull layer) treat the
# all-zeros sentinel as "no pinned key, skip pinning", which is exactly
# what the test wants. The override must apply to signature.c, not just
# the test TU, so we compile a test-specific signature object.
# Each test consumer also passes no_verify_platform=1 to
# hl_verify_startup as belt-and-suspenders.
$(BUILDDIR)/signature_testpk.o: src/hull/signature.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) \
		'-DHL_PLATFORM_PUBKEY_HEX="0000000000000000000000000000000000000000000000000000000000000000"' \
		-c -o $@ $<

$(BUILDDIR)/test_signature: $(TESTDIR)/hull/test_signature.c $(BUILDDIR)/signature_testpk.o $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(BUILDDIR)/signature_testpk.o $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) \
		$(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(CACERT_OBJ) \
		$(APP_ENTRIES_DEFAULT_OBJ) $(TEST_COMMON_LIBS)

# Release manifest sign/verify test — needs release.c + crypto
$(BUILDDIR)/test_release: $(TESTDIR)/hull/test_release.c $(RELEASE_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(RELEASE_OBJ) $(TEST_COMMON_LIBS)

# Tool registry + path helpers — standalone module, no runtime deps.
$(BUILDDIR)/test_tools_install: $(TESTDIR)/hull/test_tools_install.c $(TOOLS_INSTALL_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TOOLS_INSTALL_OBJ)

# Shared release I/O helpers (platform id, SHA-256, manifest parse,
# atomic write). Skipped on HL_ENABLE_HTTP_CLIENT=0 builds where the
# helper module isn't compiled in.
ifneq ($(HL_ENABLE_HTTP_CLIENT),0)
$(BUILDDIR)/test_release_io: $(TESTDIR)/hull/test_release_io.c $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(MBEDTLS_OBJS) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(MBEDTLS_OBJS) $(KEEL_LIB) -lm -lpthread
endif

# Verify-self helpers test. Reuses release_io.{c,h} for asset-name,
# checksum-line lookup, SHA-256, and self-path resolution. Same link
# dependencies as test_release_io.
ifneq ($(HL_ENABLE_HTTP_CLIENT),0)
$(BUILDDIR)/test_verify_self: $(TESTDIR)/hull/test_verify_self.c $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(MBEDTLS_OBJS) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(MBEDTLS_OBJS) $(KEEL_LIB) -lm -lpthread
endif

# Platform-sig helpers — reuses release.c (sign/verify) +
# release_io.c (find_checksum), so the test pulls those plus their
# transitive crypto deps. Available on all builds.
ifneq ($(HL_ENABLE_HTTP_CLIENT),0)
$(BUILDDIR)/test_platform_sig: $(TESTDIR)/hull/test_platform_sig.c $(PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(TEST_COMMON_LIBS)
endif

# Static file serving test — needs static middleware + vfs + keel
$(BUILDDIR)/test_static: $(TESTDIR)/hull/test_static.c $(STATIC_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(STATIC_OBJ) $(TEST_COMMON_LIBS)

# VFS test — standalone module, no runtime deps
$(BUILDDIR)/test_vfs: $(TESTDIR)/hull/test_vfs.c $(VFS_OBJ) $(PATH_NORM_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(VFS_OBJ) $(PATH_NORM_OBJ)

# SBOM test — exercises the data table + all four format functions +
# embedded-blob SHA-256 cache. Links against sbom.o + cacert.o + mbedTLS;
# nothing else. If SBOM accidentally pulls in other Hull subsystems,
# this link line will need to grow — that's the orthogonality canary.
$(BUILDDIR)/test_sbom: $(TESTDIR)/hull/test_sbom.c $(SBOM_OBJ) $(CACERT_OBJ) $(SH_JSON_OBJ) $(SH_ARENA_OBJ) $(MBEDTLS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(SBOM_OBJ) $(CACERT_OBJ) $(SH_JSON_OBJ) $(SH_ARENA_OBJ) $(MBEDTLS_OBJS)

# Path-normalize test — standalone, exercises hl_path_normalize directly
# so a regression in the helper is caught here rather than only via the
# runtime module loaders that consume it.
$(BUILDDIR)/test_path_normalize: $(TESTDIR)/hull/test_path_normalize.c $(PATH_NORM_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(PATH_NORM_OBJ)

# Async backend tests — exercise the HlAsyncBackend vtable.
# test_async_backend covers whichever backend hl_async_backend() returns
# (keel on HTTP=1, poll on HTTP=0). test_async_backend_poll always
# pins the poll backend by name, so it runs on both build flavors.
$(BUILDDIR)/test_async_backend: $(TESTDIR)/hull/test_async_backend.c $(ASYNC_BACKEND_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(ASYNC_BACKEND_OBJS) $(KEEL_LIB) -lm -lpthread

$(BUILDDIR)/test_async_backend_poll: $(TESTDIR)/hull/test_async_backend_poll.c $(ASYNC_BACKEND_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(ASYNC_BACKEND_OBJS) $(KEEL_LIB) -lm -lpthread

# Module registry — standalone, only links the registry object
$(BUILDDIR)/test_module_registry: $(TESTDIR)/hull/test_module_registry.c $(MODULE_REGISTRY_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(MODULE_REGISTRY_OBJ)

# Module resolver — needs the registry plus the manifest types
$(BUILDDIR)/test_module_resolver: $(TESTDIR)/hull/test_module_resolver.c $(MODULE_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(MODULE_OBJ)

# CA bundle test — links against cacert.o and mbedTLS for parse verification
$(BUILDDIR)/test_cacert: $(TESTDIR)/hull/test_cacert.c $(CACERT_OBJ) $(MBEDTLS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(CACERT_OBJ) $(MBEDTLS_OBJS)

test: $(TEST_BINS)
	@echo "Running tests..."
	@pass=0; fail=0; total=0; \
	for t in $(TEST_BINS); do \
		total=$$((total + 1)); \
		echo "=== $$(basename $$t) ==="; \
		if HULL_QUIET_AOT=1 $$t; then \
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
# Vendor TUs: keep MSan (we still want shadow tracking for uninitialized
# reads escaping into Hull code) but drop UBSan. The vendored crypto and
# JS interpreters have well-known "technically UB but works on every
# target" patterns (left shift of negative values in tweetnacl, function-
# pointer casts in quickjs); UBSan flags them as runtime errors, fails
# the CI job, and tells us nothing about Hull's own code. Hull's own
# CFLAGS above still get -fsanitize=memory,undefined.
QJS_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
              -DCONFIG_VERSION=\"$(QJS_VERSION)\" -DCONFIG_BIGNUM -D_GNU_SOURCE
CFLAGS    += -DHL_QJS_VERSION=\"$(QJS_VERSION)\"
LUA_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
              -DLUA_USE_POSIX
SQLITE_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
                 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5
LOG_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
              -DLOG_USE_COLOR
SH_ARENA_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer
SH_JSON_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer
TWEETNACL_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer
STB_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer
# mbedTLS must be MSan-instrumented too: it writes to caller buffers
# (e.g. mbedtls_sha256 → uint8_t digest[32]). Without instrumentation
# MSan can't see those writes and flags every subsequent read of the
# caller buffer as use-of-uninitialized-value. (Hit by
# test_release_io's sha256_hex_empty.) Preserve the -I paths and
# -DMBEDTLS_CONFIG_FILE from the non-MSan defaults at line 219.
MBEDTLS_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
                  -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library -I$(MBEDTLS_DIR) \
                  -DMBEDTLS_CONFIG_FILE='"hull_config.h"'
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
ifeq ($(HL_ENABLE_DB),1)
  CFLAGS += -DHL_ENABLE_DB
endif
# Re-add HTTP server/client defines. The Makefile-level
# NET_BACKEND_SRCS gate above keys off $(HL_ENABLE_HTTP_SERVER),
# while async/poll.c's #ifndef HL_ENABLE_HTTP_SERVER stubs key off
# the C macro. If the two disagree the linker sees duplicate
# definitions of hl_net_op_suspend / hl_net_op_complete (net/keel.o
# vs. async/poll.o). Keep them aligned for sanitizer builds.
ifeq ($(HL_ENABLE_HTTP_SERVER),1)
  CFLAGS += -DHL_ENABLE_HTTP_SERVER
endif
ifeq ($(HL_ENABLE_HTTP_CLIENT),1)
  CFLAGS += -DHL_ENABLE_HTTP_CLIENT
endif
ifneq ($(HL_ENABLE_HTTP_ANY),0)
  CFLAGS += -DHL_ENABLE_HTTP
endif
# Re-add version string (the := above clobbers earlier += additions)
CFLAGS += -DHL_VERSION=\"$(HL_VERSION)\"

# Re-add header-dependency tracking (the := blocks above clobbered the
# DEPFLAGS additions from the standard CFLAGS section).
CFLAGS           += $(DEPFLAGS)
QJS_CFLAGS       += $(DEPFLAGS)
LUA_CFLAGS       += $(DEPFLAGS)
SQLITE_CFLAGS    += $(DEPFLAGS)
LOG_CFLAGS       += $(DEPFLAGS)
SH_ARENA_CFLAGS  += $(DEPFLAGS)
SH_JSON_CFLAGS   += $(DEPFLAGS)
TWEETNACL_CFLAGS += $(DEPFLAGS)
STB_CFLAGS       += $(DEPFLAGS)
endif

msan:
	$(MAKE) clean
	$(MAKE) -C $(KEEL_DIR) clean
	$(MAKE) -C $(KEEL_DIR) CC=clang \
		KEEL_TLS=mbedtls MBEDTLS_CONFIG_FILE=hull_config.h \
		KEEL_COMPRESS=miniz MINIZ_DIR=$(CURDIR)/$(MINIZ_DIR)
	$(MAKE) CC=clang MSAN=1 test

# ── E2E tests ──────────────────────────────────────────────────────

e2e: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e.sh

e2e-build:
	sh tests/e2e_build.sh

e2e-http: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_http.sh

e2e-multipart: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_multipart.sh

e2e-attachment: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_attachment.sh

e2e-blob: $(BUILDDIR)/hull
	sh tests/e2e_blob.sh

e2e-hypermedia-photos-upload: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_hypermedia_photos_upload.sh

e2e-jwt-asym: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_jwt_asym.sh

e2e-oauth: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_oauth.sh

e2e-totp: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_totp.sh

e2e-auth-flows: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_auth_flows.sh

e2e-auth-flows-2fa: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_auth_flows_2fa.sh

e2e-auth-flows-hardening: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_auth_flows_hardening.sh

e2e-sign-in-events: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_sign_in_events.sh

e2e-sandbox: $(BUILDDIR)/hull
	sh tests/e2e_sandbox.sh

e2e-examples: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_examples.sh

# CLI-mode (app.main) examples: invoke + check stdout/exit. Distinct
# from e2e-examples which is HTTP-focused.
e2e-cli: $(BUILDDIR)/hull
	HULL_BIN=$(BUILDDIR)/hull sh tests/e2e_cli.sh

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

e2e-compute-dev: $(BUILDDIR)/hull
	sh tests/e2e_compute_dev.sh

# AOT artifact cache (requires wamrc — skipped cleanly when absent).
e2e-aot-cache: $(BUILDDIR)/hull
	sh tests/e2e_aot_cache.sh

# `hull cache list|prune|clear` + HULL_CACHE_DIR isolation.
e2e-cache: $(BUILDDIR)/hull
	sh tests/e2e_cache.sh

# Concurrent-writer stress test: N hull processes hammer the same
# cache root. Slow (~30s, spawns ~16 hull instances) — kept out of
# the default `make e2e` runs; CI invokes explicitly.
e2e-cache-concurrent: $(BUILDDIR)/hull
	sh tests/e2e_cache_concurrent.sh

# Run the cache e2e suite against a cosmopolitan-built hull. Slow
# (rebuilds platform + hull with cosmocc). CI invokes on a Linux
# x86_64 runner. The wrapper script verifies the binary is a cosmo
# APE, then delegates to e2e_cache.sh with HULL overridden.
e2e-cache-cosmo:
	@command -v cosmocc >/dev/null 2>&1 || { \
		echo "SKIP: cosmocc not on PATH"; exit 0; }
	$(MAKE) platform-cosmo
	$(MAKE) clean
	$(MAKE) CC=cosmocc EMBED_PLATFORM=cosmo -j8
	HULL=$(BUILDDIR)/hull sh tests/e2e_cache_cosmo.sh

e2e-tcc: $(BUILDDIR)/hull $(BUILDDIR)/libhull_platform.a
	sh tests/e2e_tcc.sh

e2e-install:
	sh tests/e2e_install.sh

e2e-ca-bundle: $(BUILDDIR)/hull $(BUILDDIR)/test_cacert
	sh tests/e2e_ca_bundle.sh

e2e-update: $(BUILDDIR)/hull
	sh tests/e2e_update.sh

# Tools install (hermetic: HOME redirected, no network in fast path).
e2e-tools: $(BUILDDIR)/hull
	sh tests/e2e_tools.sh

# ── TUI e2e (smoke + interactive PTY-driven) ───────────────────────
#
# The interactive part shells out to the e2e_tui_drive helper which
# spawns the hull binary under a PTY and feeds it scripted input.
# Built only when HL_ENABLE_TUI is on (no point otherwise).

ifeq ($(HL_ENABLE_TUI),1)
$(BUILDDIR)/e2e_tui_drive: $(TESTDIR)/e2e_tui_drive.c | $(BUILDDIR)
	$(CC) -std=c11 -Wall -Wextra -O2 -o $@ $<
E2E_TUI_DEPS := $(BUILDDIR)/hull $(BUILDDIR)/e2e_tui_drive
else
E2E_TUI_DEPS := $(BUILDDIR)/hull
endif

e2e-tui: $(E2E_TUI_DEPS)
	sh tests/e2e_tui.sh

hull-test-examples: $(BUILDDIR)/hull
	@for dir in examples/hello examples/rest_api examples/bench_db examples/auth \
	            examples/jwt_api examples/crud_with_auth examples/middleware examples/webhooks \
	            examples/entry examples/timers; do \
		echo "=== hull test $$dir ===" && \
		output=$$($(BUILDDIR)/hull test "$$dir" 2>&1; true) && \
		echo "$$output" && \
		if echo "$$output" | grep -qE "[0-9]+ failed"; then exit 1; fi; \
	done

# ── Self-build (hull → hull2 → hull3 chain) ─────────────────────────

self-build: $(BUILDDIR)/hull platform
	@echo "=== Self-build: hull -> hull2 -> hull3 ==="
	@# --no-verify-platform: this hull is built without EMBED_PLATFORM=1
	@# (the dev/CI default) so it has no embedded signed manifest and
	@# can't satisfy the v0.1.3 platform-sig cross-check. Self-build is
	@# verifying the build pipeline itself, not the trust chain.
	@TMPDIR=$$(mktemp -d) && \
	$(BUILDDIR)/hull build --no-verify-platform -o "$$TMPDIR/hull2" tests/fixtures/null_app && \
	"$$TMPDIR/hull2" keygen "$$TMPDIR/key" && test -f "$$TMPDIR/key.pub" && \
	"$$TMPDIR/hull2" build --no-verify-platform -o "$$TMPDIR/hull3" tests/fixtures/null_app && \
	"$$TMPDIR/hull3" keygen "$$TMPDIR/key2" && test -f "$$TMPDIR/key2.pub" && \
	echo "PASS: self-build chain verified (hull -> hull2 -> hull3)" && \
	rm -rf "$$TMPDIR" || \
	(echo "FAIL: self-build chain" && rm -rf "$$TMPDIR" && exit 1)

# ── Reproducibility check (byte-identical builds) ───────────────────
# Tests the MANIFESTO claim "same source + same hull version = same binary"
# by having hull build the same fixture twice and verifying the outputs
# are byte-identical. This is a stronger property than self-build:
# self-build proves hull can bootstrap, this proves `hull build` is
# deterministic for any given input.
#
# Methodology note: both builds use the SAME output path. macOS ld64
# hashes the output path into LC_UUID; building to different paths
# yields different UUIDs even with identical content. Building to the
# same path, then copying aside, isolates the question we actually care
# about: "is the link output deterministic for fixed inputs?" Same trick
# works on Linux (it's a no-op there since GNU ld doesn't path-hash).
#
# Forces `--compiler=system` so the test exercises the gcc/clang
# backend's `sys_compile`, which passes `-ffile-prefix-map=<srcdir>=.`
# to strip the per-build random tempdir from the .o file's embedded
# source-name. TCC's compile path doesn't yet have the equivalent
# (its `-ffile-prefix-map` support is patchy), so TCC-default builds
# can still produce per-tempdir .o variance on Linux. TCC-mode
# determinism is a separate, smaller-impact follow-up.
reproducible-check: $(BUILDDIR)/hull platform
	@echo '=== Reproducibility: byte-identical hull build outputs ==='
	@TMPDIR=$$(mktemp -d) && \
	OUT="$$TMPDIR/app" && \
	$(BUILDDIR)/hull build --compiler=system --no-verify-platform -o "$$OUT" tests/fixtures/null_app && \
	cp "$$OUT" "$$TMPDIR/snap1" && \
	$(BUILDDIR)/hull build --compiler=system --no-verify-platform -o "$$OUT" tests/fixtures/null_app && \
	cp "$$OUT" "$$TMPDIR/snap2" && \
	if cmp -s "$$TMPDIR/snap1" "$$TMPDIR/snap2"; then \
		echo "PASS: builds are byte-identical ($$(wc -c < "$$TMPDIR/snap1") bytes)"; \
		rm -rf "$$TMPDIR"; \
	else \
		echo "FAIL: builds differ"; \
		ls -l "$$TMPDIR/snap1" "$$TMPDIR/snap2"; \
		shasum -a 256 "$$TMPDIR/snap1" "$$TMPDIR/snap2" 2>/dev/null || true; \
		rm -rf "$$TMPDIR"; \
		exit 1; \
	fi

# ── Full check (sanitized build + test + e2e) ───────────────────────

check:
	$(MAKE) clean
	$(MAKE) DEBUG=1 all test e2e

# ── Static analysis ─────────────────────────────────────────────────

analyze:
	$(MAKE) clean
	$(MAKE) $(VEND_OBJS) $(MBEDTLS_OBJS) $(MINIZ_OBJ) $(SQLITE_OBJ) $(LOG_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS) $(WAMR_OBJS) $(KEEL_LIB)
ifeq ($(HL_ENABLE_TCC),1)
	# Pre-build tcc + embedded_tcc.h so vendor/tcc source isn't analyzed
	$(MAKE) $(EMBEDDED_TCC_H)
endif
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
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/agent/*.c \
		--suppress=knownArgument:$(SRCDIR)/hull/agent/overview.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/commands/tools.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/cap/wasm.c \
		--suppress=unusedStructMember:$(SRCDIR)/hull/agent/*.c \
		--suppress=unusedVariable:$(SRCDIR)/hull/agent/*.c \
		--suppress=unusedStructMember:$(SRCDIR)/hull/app_context.c \
		--suppress=unusedVariable:$(SRCDIR)/hull/app_context.c \
		--suppress=variableScope:$(SRCDIR)/hull/cap/wasm.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/runtime/js/mod_http_client.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/runtime/js/mod_compute.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/runtime/js/mod_gpu.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/runtime/lua/mod_http_client.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/runtime/lua/mod_gpu.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/runtime/js/mod_app.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/runtime/lua/mod_app.c \
		--suppress=knownConditionTrueFalse:$(SRCDIR)/hull/cap/crypto.c \
		--suppress=variableScope:$(SRCDIR)/hull/cap/test.c \
		--suppress=variableScope:$(SRCDIR)/hull/runtime/js/mod_app.c \
		--suppress=variableScope:$(SRCDIR)/hull/runtime/js/mod_test.c \
		--suppress=unmatchedSuppression \
		--suppress='*:$(QJS_DIR)/*' \
		--suppress='*:$(LUA_DIR)/*' \
		--suppress='*:$(SQLITE_DIR)/*' \
		--suppress='*:$(LOG_DIR)/*' \
		--error-exitcode=1 \
		-DHL_QJS_VERSION=\"$(QJS_VERSION)\" \
		-I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(SQLITE_DIR) -I$(KEEL_INC) \
		$(SRCDIR)/hull/main.c $(SRCDIR)/hull/alloc.c $(SRCDIR)/hull/static.c $(SRCDIR)/hull/app_context.c $(SRCDIR)/hull/agent/*.c $(SRCDIR)/hull/agent_api.c $(SRCDIR)/hull/cap/*.c \
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

# Blob storage R/W throughput benchmark (cap-layer; bypasses bindings)
$(BUILDDIR)/bench_blob: bench/blob/bench_blob.c $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ \
		bench/blob/bench_blob.c $(TEST_COMMON_LIBS)

bench-blob: $(BUILDDIR)/bench_blob
	$(BUILDDIR)/bench_blob

# Lua bytecode cache cold/warm microbench
$(BUILDDIR)/bench_bytecode_cache: bench/bytecode_cache/bench_bytecode_cache.c $(BUILDDIR)/lua_rt_bytecode_cache.o $(TEST_COMMON_DEPS) $(LUA_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ \
		bench/bytecode_cache/bench_bytecode_cache.c \
		$(BUILDDIR)/lua_rt_bytecode_cache.o \
		$(TEST_COMMON_LIBS) $(LUA_OBJS)

bench-bytecode-cache: $(BUILDDIR)/bench_bytecode_cache
	$(BUILDDIR)/bench_bytecode_cache

# QuickJS bytecode cache cold/warm microbench (JS-side parity).
$(BUILDDIR)/bench_js_bytecode_cache: bench/bytecode_cache/bench_js_bytecode_cache.c $(BUILDDIR)/js_bytecode_cache.o $(TEST_COMMON_DEPS) $(QJS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ \
		bench/bytecode_cache/bench_js_bytecode_cache.c \
		$(BUILDDIR)/js_bytecode_cache.o \
		$(TEST_COMMON_LIBS) $(QJS_OBJS)

bench-js-bytecode-cache: $(BUILDDIR)/bench_js_bytecode_cache
	$(BUILDDIR)/bench_js_bytecode_cache

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

# ── API documentation (two-tier: source comments + generated HTML) ──
#
# Ground truth: Doxygen/JSDoc/LDoc comments in the source. Generated
# browseable HTML lives under build/api/ (gitignored). Curated narrative
# docs live at docs/api/{c,lua,js}.md (hand-edited; kept in sync).
#
# Requirements:
#   - doxygen (brew install doxygen / apt install doxygen)
#   - ldoc    (luarocks install ldoc)
#   - jsdoc   (npm install -g jsdoc)
#
# Targets:
#   docs-api          — build all three
#   docs-api-c        — Doxygen → build/api/c/
#   docs-api-lua      — LDoc    → build/api/lua/
#   docs-api-js       — JSDoc   → build/api/js/
#   docs-api-check    — fail if any required tool is missing

DOXYGEN ?= doxygen
LDOC    ?= ldoc
JSDOC   ?= jsdoc

.PHONY: docs-api docs-api-c docs-api-lua docs-api-js docs-api-check

docs-api: docs-api-c docs-api-lua docs-api-js
	@echo ""
	@echo "=== Hull API docs built ==="
	@echo "  C   → build/api/c/index.html"
	@echo "  Lua → build/api/lua/index.html"
	@echo "  JS  → build/api/js/index.html"
	@echo ""
	@echo "Curated markdown reference: docs/api/{c,lua,js}.md"

docs-api-c:
	@if ! command -v $(DOXYGEN) >/dev/null 2>&1; then \
		echo "ERROR: doxygen not found. Install with: brew install doxygen / apt install doxygen"; \
		exit 1; \
	fi
	@mkdir -p build/api/c
	@$(DOXYGEN) config/Doxyfile

docs-api-lua:
	@if ! command -v $(LDOC) >/dev/null 2>&1; then \
		echo "ERROR: ldoc not found. Install with: luarocks install ldoc"; \
		exit 1; \
	fi
	@mkdir -p build/api/lua
	@$(LDOC) -c config/ldoc.cfg .

docs-api-js:
	@if ! command -v $(JSDOC) >/dev/null 2>&1; then \
		echo "ERROR: jsdoc not found. Install with: npm install -g jsdoc"; \
		exit 1; \
	fi
	@mkdir -p build/api/js
	@$(JSDOC) -c config/jsdoc.conf.json

docs-api-check:
	@missing=0; \
	for tool in $(DOXYGEN) $(LDOC) $(JSDOC); do \
		if ! command -v $$tool >/dev/null 2>&1; then \
			echo "missing: $$tool"; missing=1; \
		else \
			echo "found:   $$tool ($$($$tool --version 2>/dev/null | head -1))"; \
		fi; \
	done; \
	exit $$missing

# ── Clean ───────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR)
	@$(MAKE) -s -C $(KEEL_DIR) clean 2>/dev/null || true

# ── Header-dependency replay ────────────────────────────────────────
#
# Re-include the .d files emitted by -MMD/-MP (see DEPFLAGS above and
# the comment near the top CFLAGS definition). Each .d file is a make
# fragment listing the user headers a .c included; replaying them turns
# a header touch into the correct narrow set of .o rebuilds.
#
# `-include` (with the dash) silently ignores missing .d files on the
# first build — they appear after the first compile pass.
#
# Uses `find` rather than `wildcard` because WAMR objects live in
# nested directories under $(BUILDDIR)/wamr_core/... and shell globbing
# is the simplest portable way to gather all of them.
DEPS_ALL := $(shell find $(BUILDDIR) -name '*.d' 2>/dev/null)
-include $(DEPS_ALL)
