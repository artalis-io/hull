# Hull - Makefile
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
UNAME_M := $(shell uname -m)

# Platform axis: darwin | linux | cosmo (native Windows is future; Windows runs
# via the cosmo APE today). Selects mk/platform/<PLATFORM>.mk, which holds the
# OS-global build policy (currently just the sandbox backend: the pledge polyfill
# is Linux-only). A vendor's or feature's per-OS wiring stays inline with its
# vendor/feature fragment; only OS-GLOBAL policy lives in mk/platform/. See
# docs/build_modularization.md ("Platform axis"). COSMO is checked first because
# a cosmo build reports UNAME_S=Linux but must not pull the Linux pledge polyfill.
ifdef COSMO
PLATFORM := cosmo
else ifeq ($(UNAME_S),Darwin)
PLATFORM := darwin
else ifeq ($(UNAME_S),Linux)
PLATFORM := linux
else
PLATFORM := unknown
endif

# Retry knob for network fetches (toolchains + vendored assets). --retry-all-errors
# also retries partial transfers (curl exit 18), the observed CI download flake.
CURL_RETRY := --retry 3 --retry-all-errors --retry-delay 2
# For LARGE binary downloads (DuckDB ~127 MB, wgpu, cosmocc): --retry alone does
# not catch a *stalled* transfer (slow-but-not-erroring), which is the recurring
# CI flake. Add a connect timeout + stall detection (--speed-limit/--speed-time:
# abort, then retry, if throughput drops below 4 KB/s for 30s) and more attempts.
CURL_RETRY_LARGE := --retry 5 --retry-all-errors --retry-delay 3 \
                    --connect-timeout 30 --speed-limit 4096 --speed-time 30

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
# Casts that drop `const` (or `volatile`) are a hard error in Hull code:
# they're the primitive that would launder a sealed / .rodata table back
# into something writable. Legitimate const-drops (freeing owned-but-
# const-typed memory, POSIX execvp's argv, in-place scratch) go through
# hl_free_const / hl_alloc_free_const or a documented `(T)(uintptr_t)x`.
# Vendored TUs compile with their own relaxed *_CFLAGS, so this is
# Hull-source-only.
CFLAGS  += -Werror=cast-qual
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

# Compiler/linker hardening probe layer lives in mk/hardening.mk, included here
# at the original position (CFLAGS/LDFLAGS accumulation order is load-bearing).
include mk/hardening.mk

# Reproducibility: deterministic ar archives. Without this, ar embeds
# the mtime + uid + gid of each member, so `libhull_platform.a` and
# `libkeel.a` differ between builds and the final link inherits the
# delta. GNU ar accepts the `D` flag for this; BSD ar (macOS) respects
# the `ZERO_AR_DATE=1` env var. Exporting it covers both via the
# shared toolchain envelope; harmless when the tool already defaults
# to deterministic (modern binutils does, with
# --enable-deterministic-archives configured at distro level).
export ZERO_AR_DATE := 1

# ── Sanitizer-mode inheritance ──────────────────────────────────────
# `make debug` / `make tsan` compile the objects under a sanitizer (each gets
# an instrumentation module-ctor + calls to __asan_* / __tsan_*), then a
# FOLLOW-UP `make test` in a SEPARATE invocation would relink them without the
# matching runtime: undefined __asan_init / __asan_memcpy from sh_seal_arena.o
# et al. Record the active sanitizer in a stamp so a later bare `make test`
# inherits it and links the runtime, making the documented `make debug &&
# make test` work. `make clean` drops the stamp (it lives under build/); with
# the stamp absent a bare build is a normal release build. Auto-inherited ONLY
# when no mode is given on the command line. An explicit `make DEBUG=1 test`
# (what CI runs) is self-consistent and never consults the stamp.
SANITIZER_STAMP := build/.sanitizer.mk
ifeq ($(origin DEBUG),undefined)
ifeq ($(origin TSAN),undefined)
ifeq ($(origin MSAN),undefined)
-include $(SANITIZER_STAMP)
ifdef DEBUG
$(info [make] inheriting DEBUG (ASan+UBSan) from a prior `make debug`; `make clean` for a release build)
endif
endif
endif
endif

# Build mode
ifdef DEBUG
CFLAGS += -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
# Event-loop thread-affinity tripwires (no-op in release). See
# include/hull/shared/thread_affinity.h.
CFLAGS += -DHL_THREAD_AFFINITY_CHECKS
else ifdef TSAN
# ThreadSanitizer build. Appends to the normal CFLAGS (Hull's own TUs get
# instrumented; vendor TUs stay uninstrumented, which TSan tolerates - it
# just won't flag races inside vendored code). Used to validate the
# worker-pool / shared-state paths (cap/wasm.c segments, worker_*.c).
CFLAGS += -g -O1 -fsanitize=thread -fno-omit-frame-pointer
LDFLAGS += -fsanitize=thread
CFLAGS += -DHL_THREAD_AFFINITY_CHECKS
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
HULL_VENDOR_KEEL_VERSION := $(shell git -C vendor/keel describe --tags --always 2>/dev/null || echo unknown)
HULL_VENDOR_WAMR_VERSION := $(shell git -C vendor/wamr describe --tags --always 2>/dev/null || echo unknown)
CFLAGS += -DHULL_VENDOR_KEEL_COMMIT=\"$(HULL_VENDOR_KEEL_COMMIT)\"
CFLAGS += -DHULL_VENDOR_WAMR_COMMIT=\"$(HULL_VENDOR_WAMR_COMMIT)\"
CFLAGS += -DHULL_VENDOR_KEEL_VERSION=\"$(HULL_VENDOR_KEEL_VERSION)\"
CFLAGS += -DHULL_VENDOR_WAMR_VERSION=\"$(HULL_VENDOR_WAMR_VERSION)\"

# Escape hatch to append flags from the command line, e.g.
#   make EXTRA_CFLAGS='-DHL_PLATFORM_PUBKEY_HEX="<hex>"'
# Both pubkey macros are #ifndef-guarded (release.h / signature.h), so this is
# how the composed-signature test harness pins a TEST platform key without
# editing a committed header. Empty by default; no effect on a normal build.
CFLAGS += $(EXTRA_CFLAGS)

.DEFAULT_GOAL := all

# ── Directories ──────────────────────────────────────────────────────

SRCDIR   := src
INCDIR   := include
TESTDIR  := tests
BUILDDIR := build
VENDDIR  := vendor

# ── xxd const-qualification helpers ──────────────────────────────────
#
# `xxd -i` emits `unsigned char NAME[]` and `unsigned int NAME_len`
# - both writable. Without `const` they land in `.data`; with it they
# land in `.rodata`, which the OS protects as read-only at the page
# level. Defense-in-depth against heap memory-write bugs that would
# otherwise be able to rewrite embedded modules, the CA bundle, the
# cosmo platform archives, etc., post-boot. See
# docs/security.md §4b "Sealed runtime tables".
#
# Two forms, used by ~20 xxd invocations across the Makefile:
#
#   XXD_CONST_SEAL - in-place sed for `xxd ... > FILE` rules.
#     Usage: `xxd -i $< > $@ && $(XXD_CONST_SEAL) $@ && rm -f $@.bak`
#     The .bak dance is portable across BSD + GNU sed.
#
#   XXD_CONST_PIPE - stdin-to-stdout sed for `xxd ... | ... > FILE`
#     pipelines (cosmo platform embedding, template embedding).
#     Usage: `xxd -i FILE | sed 's/.../.../g' | $(XXD_CONST_PIPE) > $@`
#
# Defined unconditionally near the top so every rule sees them
# regardless of HL_EMBED_CA_BUNDLE / HL_ENABLE_LUA / etc. (A build
# variant that omitted these once silently lost the const-
# qualification on the cacert + stdlib + cosmo-platform rules and
# the redirect failed with "permission denied" because the missing
# variable left a bare filename being execve'd.)
XXD_CONST_SEAL := sed -i.bak \
	-e 's/^unsigned char /const unsigned char /' \
	-e 's/^unsigned int /const unsigned int /'
XXD_CONST_PIPE := sed \
	-e 's/^unsigned char /const unsigned char /' \
	-e 's/^unsigned int /const unsigned int /'

# QuickJS vendored config -> mk/vendor/quickjs.mk
include mk/vendor/quickjs.mk

# Lua 5.4 vendored config -> mk/vendor/lua.mk
include mk/vendor/lua.mk

# HTTP / DB / composable-feature config flags live in mk/flags.mk, included
# here at the original position (CFLAGS += order is load-bearing).
include mk/flags.mk

# define-feature-archive macro (mk/feature.mk). Only defines a macro (no side
# effects), so it is safe to include early; the per-feature fragments below use
# it. AR_FEATURE_LIB / BUILDDIR are resolved at rule time, not here.
include mk/feature.mk

# Keel submodule config -> mk/vendor/keel.mk
include mk/vendor/keel.mk

# mbedTLS vendored config -> mk/vendor/mbedtls.mk
include mk/vendor/mbedtls.mk

# ── Database backends (SQLite embedded / PostgreSQL wire): config ──
# Two independent backends behind the HlDbBackend vtable, each with its
# own flag, so a build can enable SQLite only (default), PostgreSQL only,
# both, or neither:
#   HL_ENABLE_SQLITE   (default 1): vendored SQLite amalgamation.
#   HL_ENABLE_POSTGRES (default 0): pure-C PostgreSQL v3 wire client
#                                   (roadmap §1; docs/postgres_backend_design.md).
# HL_ENABLE_DB is the DERIVED umbrella ("any DB backend at all"),
# -D-defined iff either granular flag is on. Every `#if defined(HL_ENABLE_DB)`
# guard keeps meaning "a backend is present". Mirrors the HL_ENABLE_HTTP
# server/client split above. When the umbrella is off, cap/db*.c,
# worker_db*, migrate*, mod_db.c, agent/db.c, and SQLite itself are
# excluded (pure-compute); apps must avoid db.*, migrate.*, and any stdlib
# module that needs a DB (session, ratelimit, idempotency, outbox, inbox,
# rbac, search).
#
# Back-compat: HL_ENABLE_DB=0 still works; it pins both granular flags off.
# HL_ENABLE_SQLITE / HL_ENABLE_POSTGRES are resolved earlier (near the
# HL_LINK_TLS gate); here we just emit the -D macros and derive the umbrella.

ifeq ($(HL_ENABLE_SQLITE),1)
CFLAGS += -DHL_ENABLE_SQLITE
endif
ifeq ($(HL_ENABLE_POSTGRES),1)
CFLAGS += -DHL_ENABLE_POSTGRES
endif
ifeq ($(HL_ENABLE_MYSQL),1)
CFLAGS += -DHL_ENABLE_MYSQL
endif
ifeq ($(HL_ENABLE_DUCKDB),1)
CFLAGS += -DHL_ENABLE_DUCKDB -I$(VENDDIR)/duckdb
endif
# HL_ENABLE_VALKEY=1 compiles the Valkey/Redis KV backend (cap/respwire.c +
# cap/valkey_conn.c + cap/valkey.c) INTO the base and self-registers it via the
# strong hl_kv_feature_backends hook. Production composition is --with=valkey
# this flag is the compiled-in dev/test path. rediss:// TLS needs the
# shared TLS client (HL_LINK_TLS), pulled below.
ifeq ($(HL_ENABLE_VALKEY),1)
CFLAGS += -DHL_ENABLE_VALKEY
endif

# Derived umbrella. `override` forces the derived value even if a
# contradictory HL_ENABLE_DB=1 was passed with all backends off (resolves
# to a coherent "no backend" rather than a broken half-build). The
# back-compat check above already read the caller's HL_ENABLE_DB=0 intent.
#
# Exception: HL_SQLITE_FEATURE=1 is exactly the "DB core, backend composed" case
# (docs/sqlite_feature.md). No backend is compiled into the base, but
# the umbrella stays ON so the vtable + selector + generic db.* caps + the weak
# hl_db_feature_backends hook remain; the backend arrives from the composed
# libhull_feature-sqlite.a. This is NOT the broken half-build the override guards
# against -- it is the composable base.
ifeq ($(HL_ENABLE_SQLITE)$(HL_ENABLE_POSTGRES)$(HL_ENABLE_MYSQL)$(HL_ENABLE_DUCKDB),0000)
ifeq ($(HL_SQLITE_FEATURE),1)
override HL_ENABLE_DB := 1
CFLAGS += -DHL_ENABLE_DB
else
override HL_ENABLE_DB := 0
endif
else
override HL_ENABLE_DB := 1
CFLAGS += -DHL_ENABLE_DB
endif

# HL_ENABLE_POSTGRES=1 links the pure-C wire client (cap/pgwire.c +
# cap/pg_conn.c + cap/db_postgres.c). Plaintext (trust /
# cleartext auth); TLS + SCRAM additionally require mbedTLS,
# which is only linked today when an HTTP half is on. See
# docs/postgres_backend_design.md.

# ── HTTP server / client - config flag reference ────────────────────
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
#   1       1       Full HTTP - default Hull build.          ~5.0 MB
#   0       1       CLI + outbound HTTPS. No server stack;   ~4.9 MB
#                   http.fetch works, can hit https://*.
#                   Keel + mbedTLS still linked.
#   1       0       Inbound server, no outbound HTTP. Niche  ~5.0 MB
#                   - server-only apps that may not make
#                   outgoing HTTP calls.
#   0       0       Pure compute / CLI. No Keel, no mbedTLS. ~4.4 MB
#
# HL_ENABLE_HTTP back-compat:
#   Setting HL_ENABLE_HTTP=0 still works - it pins both
#   HL_ENABLE_HTTP_{SERVER,CLIENT} to 0. Setting HL_ENABLE_HTTP=1
#   (the default) leaves the granular flags at their own defaults
#   (both 1), so existing `make` invocations don't change behavior.
#
# Linker dependencies:
#   mbedTLS and libkeel.a are linked when EITHER server or client
#   is on (Keel ships both halves; the linker dead-strips the unused
#   side). The compile-time -DHL_ENABLE_HTTP macro stays defined in
#   that same case, so existing source guards continue to mean "any
#   HTTP at all" - granular guards (HL_ENABLE_HTTP_{SERVER,CLIENT})
#   are only used where the distinction matters.

# SQLite vendored config -> mk/vendor/sqlite.mk
include mk/vendor/sqlite.mk

# log.c vendored config -> mk/vendor/log.mk
include mk/vendor/log.mk

# sh_arena vendored config -> mk/vendor/sh_arena.mk
include mk/vendor/sh_arena.mk

# sh_json vendored config -> mk/vendor/sh_json.mk
include mk/vendor/sh_json.mk

# tweetnacl vendored config -> mk/vendor/tweetnacl.mk
include mk/vendor/tweetnacl.mk

# ── HL_ENABLE_IMAGE (image codecs, on by default) ─────────────────────
# The image decode/encode subsystem: cap/image.c + cap/image_stb.c + the
# per-runtime mod_image bindings + vendored stb_image. On by default (web apps
# want avatars / thumbnails), so slimming it out is SUBTRACTIVE -- a flavor knob
# like HL_ENABLE_DB, not a `--with=` feature. `make HL_ENABLE_IMAGE=0` drops the
# codec subsystem and stb entirely for a compute / CLI / signing binary that
# never touches images; `hull/image` then needs an optional `"hull/image@1?"`
# declaration to resolve on such a build (a non-optional decl is a hard error,
# the HL_ENABLE_DB/GPU pattern). stb_image is image's ONLY consumer, so it goes
# too. See CLAUDE.md "Image-less builds".
HL_ENABLE_IMAGE ?= 1
ifeq ($(HL_ENABLE_IMAGE),1)
CFLAGS += -DHL_ENABLE_IMAGE
endif

# stb vendored config -> mk/vendor/stb.mk
include mk/vendor/stb.mk

# unicode tables vendored config -> mk/vendor/unicode.mk
include mk/vendor/unicode.mk

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

# WASM/WAMR vendored runtime -> mk/vendor/wamr.mk
include mk/vendor/wamr.mk

# ── Link-Time Optimisation (LTO) ─────────────────────────────────────
#
# Opt-in cross-TU optimisation.  Disabled by default because:
#   - Clean build time roughly doubles (~54s vs ~27s on Apple M).
#   - Binary size grows ~3.5% (LLVM ThinLTO metadata).
#   - Cosmocc doesn't accept -flto (probe fails silently → no-op).
#
# Enable with: make HL_ENABLE_LTO=1
#
# What it does: every TU (Hull + every vendor: mbedtls, sqlite, lua,
# qjs, miniz, tweetnacl, wamr, log.c, sh_arena, sh_json, plus Keel's
# own + its vendored llhttp) gets -flto=thin; the final link gets the
# same.  Lets the linker inline + dead-strip across TU boundaries and
# is the prerequisite for meaningful clang -fsanitize=cfi-icall in a
# future patch (per-call-site indirect-call type checks).
#
# Phase 1 audit (2026-06-19, Apple clang 17 arm64) confirmed clean
# across all vendor TUs including byte-reproducible builds.  See
# docs/security.md § 4c for the threat-model framing.
# HL_ENABLE_CFI implies HL_ENABLE_LTO (CFI needs LTO bitcode).
ifeq ($(HL_ENABLE_CFI),1)
  HL_ENABLE_LTO := 1
endif

ifeq ($(HL_ENABLE_LTO),1)
  # Self-contained probe (doesn't depend on the hardening block's
  # hl_have_cflag, which is gated on HULL_DISABLE_HARDENING).
  HL_LTO_CFLAG := $(shell tmp="$$(mktemp 2>/dev/null || echo /tmp/hlltoprobe$$$$.o)"; \
      printf 'int main(void){return 0;}\n' \
      | $(CC) -Werror -flto=thin -x c -c -o "$$tmp" - >/dev/null 2>&1 \
      && echo "-flto=thin"; rm -f "$$tmp")
  ifeq ($(HL_LTO_CFLAG),)
    HL_LTO_CFLAG := $(shell tmp="$$(mktemp 2>/dev/null || echo /tmp/hlltoprobe$$$$.o)"; \
        printf 'int main(void){return 0;}\n' \
        | $(CC) -Werror -flto -x c -c -o "$$tmp" - >/dev/null 2>&1 \
        && echo "-flto"; rm -f "$$tmp")
  endif
  ifneq ($(HL_LTO_CFLAG),)
    # LTO bitcode archives need a bitcode-aware ar (default GNU ar
    # only indexes ELF symbols → libkeel.a's bitcode objects look
    # unreachable to the linker).  Probe for llvm-ar / llvm-ar-N
    # (Ubuntu installs the suffixed name in /usr/bin).  Apple's ar
    # is already bitcode-aware via libtool so darwin doesn't need it.
    HL_LTO_AR := $(or \
        $(shell command -v llvm-ar 2>/dev/null),\
        $(shell command -v llvm-ar-21 2>/dev/null),\
        $(shell command -v llvm-ar-20 2>/dev/null),\
        $(shell command -v llvm-ar-19 2>/dev/null),\
        $(shell command -v llvm-ar-18 2>/dev/null),\
        $(shell command -v llvm-ar-17 2>/dev/null),\
        $(shell command -v llvm-ar-16 2>/dev/null))
    ifneq ($(HL_LTO_AR),)
      AR := $(HL_LTO_AR)
    endif
    CFLAGS           += $(HL_LTO_CFLAG)
    LDFLAGS          += $(HL_LTO_CFLAG)
    QJS_CFLAGS       += $(HL_LTO_CFLAG)
    LUA_CFLAGS       += $(HL_LTO_CFLAG)
    MBEDTLS_CFLAGS   += $(HL_LTO_CFLAG)
    SQLITE_CFLAGS    += $(HL_LTO_CFLAG)
    LOG_CFLAGS       += $(HL_LTO_CFLAG)
    SH_ARENA_CFLAGS  += $(HL_LTO_CFLAG)
    SH_JSON_CFLAGS   += $(HL_LTO_CFLAG)
    TWEETNACL_CFLAGS += $(HL_LTO_CFLAG)
    STB_CFLAGS       += $(HL_LTO_CFLAG)
    ifeq ($(HL_ENABLE_WASM),1)
      WAMR_CFLAGS    += $(HL_LTO_CFLAG)
    endif
    # Keel passthrough is wired at the Keel sub-make invocation
    # (KEEL_EXTRA_CFLAGS / KEEL_EXTRA_LDFLAGS).
  else
    $(warning HL_ENABLE_LTO=1 but $(CC) accepts neither -flto=thin nor -flto; building without LTO)
  endif
endif

# ── Control-Flow Integrity (CFI) ─────────────────────────────────────
#
# Opt-in clang `-fsanitize=cfi-icall`.  At every indirect call site,
# verifies the loaded function pointer's runtime type matches the call
# site's expected signature.  If a heap-write lands a fake function
# pointer inside an unsealed per-request struct, CFI catches the type
# mismatch and traps.
#
# Together with the sealed-arena work (router vtable, KlAllocator,
# KlConfig callbacks, mbedTLS deep allocations): the seal defends the
# static dispatch tables; CFI defends dynamic per-request callback
# slots.  Same ROP/JOP escalation chain, two stages.
#
# Enable with: make HL_ENABLE_CFI=1
# (Auto-enables HL_ENABLE_LTO=1; CFI needs LTO for cross-TU coverage.)
#
# Platform support:
#   - Linux clang ≥ 7.0     ✓ (the intended target)
#   - Linux gcc             ✗ (gcc has no -fsanitize=cfi)
#   - macOS Apple clang     ✗ (LLVM CFI runtime not on Darwin)
#   - Cosmopolitan          ✗ (no -fsanitize=cfi support)
#
# On unsupported toolchains the probe rejects the flag → no-op.
#
# Vendor TU exclusions (Phase 3, 2026-06-20):
#
# 1. QuickJS (QJS_CFLAGS): QJS registers C callbacks via
#    JS_NewCFunctionMagic((JSCFunctionMagic *)f) where the underlying
#    function may be 0/1/2-arg, magic, or magic+ctor - disparate
#    signatures cast through a generic prototype.  Vendor-internal
#    pattern Hull doesn't control without patching qjs.
#
# 2. WAMR (WAMR_CFLAGS): WAMR registers host imports as NativeSymbol
#    entries with a generic typed-erased dispatcher; concrete handler
#    signatures vary per import.  Vendor-internal pattern.
#
# Both vendors still get -flto=thin and -fsplit-lto-unit so they
# co-link cleanly with the CFI-on TUs (clang refuses mixed LTO links
# otherwise).
#
# Hull-side coverage: cap/* (including the post-2026-06-21 typed-
# handle HlGpuBackend) + commands/* + Hull's own runtime/* +
# worker_* + sh_* + lua + mbedtls + sqlite + tweetnacl + miniz +
# log.c + Keel (via KEEL_EXTRA_* passthrough).  ~85% of indirect
# call sites in the final binary.  All six Hull polymorphic vtables
# (HlDbBackend, HlGpuBackend, HlAsyncBackend, HlNetBackend,
# HlCompilerVtable, HlRuntimeVtable) use typed-handle method
# signatures so CFI sees a matching type-id at every dispatch site.
#
# What this defends: a heap-write that lands a fake fn ptr in any
# CFI-covered TU's vtable / callback slot faults at the indirect
# call site instead of pivoting control flow.
#
# What it doesn't defend: QJS / WAMR vendor-internal call paths
# (see vendor exclusions above), and user-supplied callback
# contexts where the user owns the type (HlRowCallback,
# HlAsyncTimerFn, HlAsyncWorkFn etc. - intentional `void *cb_ctx`).
#
# See docs/security.md § 4c "Relationship to §4b" and
# docs/roadmap_next.md § 9 for the full design + spike history.
ifeq ($(HL_ENABLE_CFI),1)
  # Probe -fsanitize=cfi-icall.  Requires LTO bitcode at compile
  # time, so probe with -flto=thin too - a compiler that accepts
  # cfi-icall but rejects LTO is no use here.
  HL_CFI_CFLAG := $(shell tmp="$$(mktemp 2>/dev/null || echo /tmp/hlcfiprobe$$$$.o)"; \
      printf 'int main(void){return 0;}\n' \
      | $(CC) -Werror -flto=thin -fsanitize=cfi-icall \
              -x c -c -o "$$tmp" - >/dev/null 2>&1 \
      && echo "-fsanitize=cfi-icall"; rm -f "$$tmp")
  ifneq ($(HL_CFI_CFLAG),)
    # Trap-on-violation in release; recover-with-diagnostic in debug.
    # Release behaviour defends; debug behaviour lets developers see
    # which call site CFI flagged.
    ifdef DEBUG
      HL_CFI_MODE := -fno-sanitize-trap=cfi -fsanitize-recover=cfi
    else
      HL_CFI_MODE := -fsanitize-trap=cfi
    endif
    # -fsplit-lto-unit is required across ALL LTO TUs whenever CFI
    # is on for any subset (mixed CFI / non-CFI LTO links otherwise
    # fail with "inconsistent LTO Unit splitting").  Vendor TUs that
    # opt out of CFI itself still need this flag.
    # -DHL_CFI_BUILD=1 is the compile-time signal the CFI death test
    # checks (clang's __has_feature(cfi_icall) is unreliable here).
    CFLAGS           += $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit -DHL_CFI_BUILD=1
    LDFLAGS          += $(HL_CFI_CFLAG) -fsplit-lto-unit
    LUA_CFLAGS       += $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit
    MBEDTLS_CFLAGS   += $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit
    SQLITE_CFLAGS    += $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit
    LOG_CFLAGS       += $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit
    SH_ARENA_CFLAGS  += $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit
    SH_JSON_CFLAGS   += $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit
    TWEETNACL_CFLAGS += $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit
    STB_CFLAGS       += $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit
    # QJS / WAMR: -fsplit-lto-unit only, no -fsanitize=cfi-icall
    # (see "Vendor TU exclusions" in the block comment above).
    QJS_CFLAGS       += -fsplit-lto-unit
    ifeq ($(HL_ENABLE_WASM),1)
      WAMR_CFLAGS    += -fsplit-lto-unit
    endif
    # Keel passthrough carries CFI flags via the sub-make rule
    # (KEEL_EXTRA_CFLAGS / KEEL_EXTRA_LDFLAGS).
  else
    $(warning HL_ENABLE_CFI=1 but $(CC) does not support -fsanitize=cfi-icall on this target; building without CFI (Linux clang ≥ 7.0 only))
  endif
endif

# ── HL_ENABLE_TUI - terminal UI capability (composable feature) ────
#
# TUI is a composable feature (like gpu / duckdb): the base is built
# TUI-FREE so apps that never touch the terminal link a leaner platform
# lib (cap/tui.c + tui_input.c + tui_width.c + the runtime bindings + the
# stdlib `hull/tui` module all drop out). An app that declares `hull/tui`
# composes it back in via `hull build --with=tui` (auto-inferred by
# `--flavor=auto`), whole-archive-linking libhull_feature-tui.a.
#
# The hull TOOLCHAIN binary still needs TUI for its own `--tui` dogfood
# commands (`hull doctor --tui`, `hull dev --tui`, ...). It gets them by
# force-loading the same feature archive at link time (HL_TUI_TOOLCHAIN,
# below) even though the base is TUI-free -- the exact mechanism an app
# compose uses, applied to hull itself.
#
# Native default: off (lean apps; toolchain force-loads the archive).
# Cosmo: on. A fat APE can't force-load a native feature archive, so TUI
# is compiled in; cosmo apps link the full platform lib regardless.
ifeq ($(COSMO),1)
HL_ENABLE_TUI ?= 1
else
HL_ENABLE_TUI ?= 0
endif

ifeq ($(HL_ENABLE_TUI),1)
CFLAGS += -DHL_ENABLE_TUI
endif

# HL_TUI_TOOLCHAIN - force-load libhull_feature-tui.a into the hull binary
# so its --tui commands work on a TUI-free base. Only meaningful when the
# base is TUI-free: a TUI-compiled base already carries the strong feature
# hooks, and force-loading the archive too would double-define them. So it
# defaults on only for a native TUI-off build, and is pinned off otherwise
# (cosmo, or an explicit HL_ENABLE_TUI=1 native build).
ifeq ($(HL_ENABLE_TUI),1)
HL_TUI_TOOLCHAIN := 0
else ifeq ($(COSMO),1)
HL_TUI_TOOLCHAIN := 0
else
HL_TUI_TOOLCHAIN ?= 1
endif

# HL_TUI_LINKED - code-presence gate for the toolchain-only `--tui` command
# handling (doctor/dev/agent/modules/migrate dispatchers + the tool.dev_*
# bindings). Defined iff the hull binary will carry TUI symbols: either a
# TUI-compiled base, or a TUI-free base that force-loads the feature archive.
# Equivalent to hl_tui_feature_present() for the hull binary, but usable as a
# compile-time gate (the runtime hook can't gate whether a function is emitted,
# and an always-off branch trips unused-function -Werror / cppcheck). Only the
# command TUs (toolchain-only, never in an app binary) read it, so applying it
# globally is harmless.
ifeq ($(HL_ENABLE_TUI),1)
CFLAGS += -DHL_TUI_LINKED
HULL_HAS_TUI := 1
else ifeq ($(HL_TUI_TOOLCHAIN),1)
CFLAGS += -DHL_TUI_LINKED
HULL_HAS_TUI := 1
else
HULL_HAS_TUI := 0
endif

ifeq ($(HL_TUI_TOOLCHAIN),1)
# The cap core lives in libhull_feature-tui.a; the per-runtime bridges are separate
# archives (issue #114). Force-load the cap core + ONLY the per-runtime
# bridge(s) whose VM this build actually links (the js bridge references QuickJS, so
# a lua-only hull that omits QuickJS must NOT force-load it -- it would not link).
# Mirrors the runtime VM selection (RUNTIME=all links both; lua/js link one).
TUI_TOOLCHAIN_RT_LIBS :=
ifneq ($(RUNTIME),js)
TUI_TOOLCHAIN_RT_LIBS += $(BUILDDIR)/libhull_feature-tui-lua.a
endif
ifneq ($(RUNTIME),lua)
TUI_TOOLCHAIN_RT_LIBS += $(BUILDDIR)/libhull_feature-tui-js.a
endif
TUI_TOOLCHAIN_ARCHIVE := $(BUILDDIR)/libhull_feature-tui.a $(TUI_TOOLCHAIN_RT_LIBS)
ifeq ($(UNAME_S),Darwin)
TUI_TOOLCHAIN_LDFLAGS := -Wl,-force_load,$(BUILDDIR)/libhull_feature-tui.a \
                         $(foreach lib,$(TUI_TOOLCHAIN_RT_LIBS),-Wl,-force_load,$(lib))
else
TUI_TOOLCHAIN_LDFLAGS := -Wl,--whole-archive $(BUILDDIR)/libhull_feature-tui.a \
                         $(TUI_TOOLCHAIN_RT_LIBS) -Wl,--no-whole-archive
endif
else
TUI_TOOLCHAIN_ARCHIVE :=
TUI_TOOLCHAIN_LDFLAGS :=
endif

# compiler.c - always compiled (system backend + selection)
COMPILER_OBJ := $(BUILDDIR)/compiler.o
$(COMPILER_OBJ): $(SRCDIR)/hull/compiler.c $(INCDIR)/hull/compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# obj_emit.c - compiler-free app_registry object emitter (ELF/Mach-O/COFF).
# linker_system.c - the link-only vtable (cc/ld driver). Both feed the
# compiler-free `hull build` path (docs/compiler_free_build.md).
OBJ_EMIT_OBJ      := $(BUILDDIR)/obj_emit.o
LINKER_SYSTEM_OBJ := $(BUILDDIR)/linker_system.o
LINKER_LLD_OBJ    := $(BUILDDIR)/linker_lld.o
LINKER_ZIG_OBJ    := $(BUILDDIR)/linker_zig.o
$(OBJ_EMIT_OBJ): $(SRCDIR)/hull/obj_emit.c $(INCDIR)/hull/obj_emit.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
$(LINKER_SYSTEM_OBJ): $(SRCDIR)/hull/linker_system.c $(INCDIR)/hull/linker.h $(INCDIR)/hull/obj_emit.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
# lld linker backend (Tier A) for the toolchain-free axis (docs/toolchain_free_build.md).
$(LINKER_LLD_OBJ): $(SRCDIR)/hull/linker_lld.c $(INCDIR)/hull/linker.h $(INCDIR)/hull/obj_emit.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
# zig linker backend (zig cc: toolchain-free + cross-compile).
$(LINKER_ZIG_OBJ): $(SRCDIR)/hull/linker_zig.c $(INCDIR)/hull/linker.h $(INCDIR)/hull/obj_emit.h | $(BUILDDIR)
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
# XXD_CONST_SEAL post-processes to `const`-qualify the generated
# symbols so the bundle lands in `.rodata` (see definition above).
# Also extract the "last updated" line from the PEM header and emit it as
# HL_CA_BUNDLE_DATE so `hull doctor` can display it.
$(EMBEDDED_CACERT_H): $(CACERT_PEM) | $(BUILDDIR)
	cp $(CACERT_PEM) $(BUILDDIR)/cacert.pem.tmp
	printf '\0' >> $(BUILDDIR)/cacert.pem.tmp
	xxd -i -n embedded_cacert $(BUILDDIR)/cacert.pem.tmp > $@
	@$(XXD_CONST_SEAL) $@ && rm -f $@.bak
	@date=$$(grep 'last updated on:' $(CACERT_PEM) | head -1 | sed 's/.*last updated on: *//;s/ *$$//'); \
		printf '#define HL_CA_BUNDLE_DATE "%s"\n' "$$date" >> $@
	rm -f $(BUILDDIR)/cacert.pem.tmp
endif

# ── Bundled objects for the compiler-free `hull build --no-compiler` ──
#
# app_main.o + app_feature_registry-{lua,js}.o are invariant per runtime, so
# they are compiled once here (this build's NATIVE format + arch) and
# xxd-embedded; `hull build --no-compiler` extracts the matching blob and
# links it against an emitted app_registry.o with no C compiler.
# docs/compiler_free_build.md. Disabled on cosmo: --no-compiler is dual-arch
# and unsupported there, and a fat APE .o doesn't xxd cleanly.
HL_BUNDLE_OBJS ?= 1
ifneq ($(findstring cosmo,$(CC)),)
HL_BUNDLE_OBJS := 0
endif
BUNDLED_OBJS_OBJ := $(BUILDDIR)/bundled_objs.o
ifeq ($(HL_BUNDLE_OBJS),1)
CFLAGS += -DHL_BUNDLE_OBJS
EMBEDDED_BUNDLED_H := $(BUILDDIR)/embedded_bundled_objs.h

$(BUILDDIR)/bundled_app_main.o: templates/app_main.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
$(BUILDDIR)/bundled_afr_lua.o: templates/app_feature_registry_lua.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
$(BUILDDIR)/bundled_afr_js.o: templates/app_feature_registry_js.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# xxd each bundled object into one const-sealed header (three byte arrays).
$(EMBEDDED_BUNDLED_H): $(BUILDDIR)/bundled_app_main.o $(BUILDDIR)/bundled_afr_lua.o $(BUILDDIR)/bundled_afr_js.o | $(BUILDDIR)
	xxd -i -n bundled_app_main $(BUILDDIR)/bundled_app_main.o > $@
	xxd -i -n bundled_afr_lua $(BUILDDIR)/bundled_afr_lua.o >> $@
	xxd -i -n bundled_afr_js $(BUILDDIR)/bundled_afr_js.o >> $@
	@$(XXD_CONST_SEAL) $@ && rm -f $@.bak

$(BUNDLED_OBJS_OBJ): $(SRCDIR)/hull/bundled_objs.c $(INCDIR)/hull/bundled_objs.h $(EMBEDDED_BUNDLED_H) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
else
$(BUNDLED_OBJS_OBJ): $(SRCDIR)/hull/bundled_objs.c $(INCDIR)/hull/bundled_objs.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
endif

# vendored/embedded asset refresh targets (maintenance) -> mk/fetch.mk
include mk/fetch.mk

# wgpu-native config -> mk/vendor/wgpu.mk
include mk/vendor/wgpu.mk
# DuckDB static-libs config -> mk/vendor/duckdb.mk
include mk/vendor/duckdb.mk
# ── Platform-specific policy (the sandbox backend) ─────────────────
#
# PLATFORM is computed near the top. mk/platform/linux.mk defines the
# PLEDGE_* vars + compile rule (the Linux pledge/unveil polyfill);
# darwin/cosmo are thin seams (their OS specifics live with their
# vendor/feature). PLEDGE_OBJS defaults empty on non-Linux so the hull
# link references it unconditionally.
include mk/platform/$(PLATFORM).mk
PLEDGE_OBJS ?=

# ── Hull source files ───────────────────────────────────────────────

# Capability sources. The convention: `cap/*.c` is glob-included by DEFAULT and
# reducible subsystems are SUBTRACTED below (the ifeq/ifneq `filter-out` blocks).
# So a "base cap module" - a small always-in-base C capability with no HL_ENABLE_*
# gate and no --with archive (mime, blob, tar; see CLAUDE.md "Extension taxonomy")
# - needs NO Makefile edit: dropping cap/<name>.c into the glob rides CAP_OBJS
# automatically. Only tool.c / test.c (they need runtime/linker visibility from
# the runtime bindings) and the OPTIONAL subsystems (image, the DB backends, GPU,
# HTTP halves) are filtered out. Runtime-layer test bindings live in
# runtime/{lua,js}/mod_test.c (picked up via the JS_RT_SRCS / LUA_RT_SRCS globs).
CAP_SRCS := $(filter-out $(SRCDIR)/hull/cap/tool.c $(SRCDIR)/hull/cap/test.c,$(wildcard $(SRCDIR)/hull/cap/*.c))
ifeq ($(HL_ENABLE_IMAGE),0)
  # Image codecs off: drop the codec vtable + stb backend (stb obj already
  # emptied above). The per-runtime mod_image bindings are dropped from the
  # runtime sources below.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/image.c \
      $(SRCDIR)/hull/cap/image_stb.c, \
      $(CAP_SRCS))
endif
ifeq ($(HL_ENABLE_DB),0)
  # Umbrella off (no backend): drop the shared query surface + selector +
  # the connection registry too.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/db.c \
      $(SRCDIR)/hull/cap/db_select.c \
      $(SRCDIR)/hull/cap/db_registry.c, \
      $(CAP_SRCS))
endif
ifneq ($(HL_ENABLE_SQLITE),1)
  # SQLite backend, its UDF bridge, and the SQLite stmt-cache / query engine
  # (cap/db.c) are SQLite-only. The backend-agnostic bits (DSN selection in
  # db_select.c, the _hull_* guard in db_common.c) stay compiled for a
  # Postgres-only build.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/db.c \
      $(SRCDIR)/hull/cap/db_sqlite.c \
      $(SRCDIR)/hull/cap/db_udf.c, \
      $(CAP_SRCS))
endif
ifneq ($(HL_ENABLE_POSTGRES),1)
  # PostgreSQL wire backend: codec + connection + the vtable.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/db_postgres.c \
      $(SRCDIR)/hull/cap/pg_conn.c \
      $(SRCDIR)/hull/cap/pgwire.c, \
      $(CAP_SRCS))
endif
ifneq ($(HL_ENABLE_MYSQL),1)
  # MySQL / MariaDB wire backend: codec + connection + vtable. Off by default.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/db_mysql.c \
      $(SRCDIR)/hull/cap/mysql_conn.c \
      $(SRCDIR)/hull/cap/mysqlwire.c, \
      $(CAP_SRCS))
endif
ifneq ($(HL_ENABLE_VALKEY),1)
  # Valkey/Redis KV backend (the first NON-SQL connection feature): RESP codec +
  # connection + the HlKvBackend vtable. Off by default; lives only in the
  # composed --with=valkey feature archive. The base-resident weak hook
  # (cap/kv_feature.c) and generic KV cap stay in CAP_SRCS.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/respwire.c \
      $(SRCDIR)/hull/cap/valkey_conn.c \
      $(SRCDIR)/hull/cap/valkey.c \
      $(SRCDIR)/hull/cap/valkey_register.c, \
      $(CAP_SRCS))
endif
ifneq ($(HL_ENABLE_DUCKDB),1)
  # DuckDB backend vtable (statically-linked libduckdb). Off by default.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/db_duckdb.c, \
      $(CAP_SRCS))
endif
ifneq ($(HL_ENABLE_GPU),1)
  # wgpu-native backend vtable (defines hl_gpu_backend_wgpu). Off by default.
  # Filtered out of the base entirely -- like db_duckdb.c -- NOT stub-compiled,
  # so the composable gpu feature (make feature-gpu) can supply the real
  # cap_gpu_wgpu.o without a base stub shadowing it at the compose link. The
  # generic gpu dispatch layer (cap/gpu.c) stays base-resident; only this
  # concrete backend is feature-gated.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/gpu_wgpu.c, \
      $(CAP_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_CLIENT),0)
  # CLIENT-only capability sources - http.fetch sync + async + SMTP send.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/http.c \
      $(SRCDIR)/hull/cap/http_async.c \
      $(SRCDIR)/hull/cap/smtp.c \
      $(SRCDIR)/hull/cap/smtp_op.c \
      $(SRCDIR)/hull/cap/smtp_worker.c \
      $(SRCDIR)/hull/cap/smtp_tls.c \
      $(SRCDIR)/hull/cap/smtp_transport.c, \
      $(CAP_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
  # SERVER-only capability sources - body reader (request bodies) +
  # WebSocket server. cap/test.c (in-process HTTP harness) is handled
  # separately below.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/ws.c \
      $(SRCDIR)/hull/cap/body.c, \
      $(CAP_SRCS))
endif
ifeq ($(HL_ENABLE_TUI),0)
  # Drop the TUI capability when disabled. cap/tui_width.c is dropped too: its
  # symbols (hl_tui_cp_width / hl_tui_utf8_decode) are used ONLY by cap/tui.c +
  # cap/tui_input.c, so it's dead weight in a TUI-free base and, crucially, must
  # travel WITH the tui feature archive so a whole-archive compose resolves those
  # references internally (GNU ld can't satisfy a force-loaded archive's refs from
  # an already-processed base lib). It's bundled into libhull_feature-tui.a below.
  # cap/tui_feature.c (the weak base hooks) is NOT dropped -- it stays
  # base-resident so hl_tui_feature_present always resolves.
  CAP_SRCS := $(filter-out \
      $(SRCDIR)/hull/cap/tui.c \
      $(SRCDIR)/hull/cap/tui_input.c \
      $(SRCDIR)/hull/cap/tui_width.c, \
      $(CAP_SRCS))
endif
CAP_OBJS := $(patsubst $(SRCDIR)/hull/cap/%.c,$(BUILDDIR)/cap_%.o,$(CAP_SRCS))
# host_match is a domain-free leaf util (src/hull/utils/host_match.c) shared by
# the db / http / smtp host allowlists. Its link footprint is coextensive with
# the cap layer that calls it, so it rides in CAP_OBJS (which also feeds
# TEST_CAP_OBJS + every link list) rather than needing a per-list edit. Harmless
# where unreferenced (flavors with no db and no http).
HOST_MATCH_OBJ := $(BUILDDIR)/host_match.o
CAP_OBJS += $(HOST_MATCH_OBJ)
# hex is a domain-free leaf util (src/hull/utils/hex.c): the single home for the
# byte->hex-buffer transform (H1 / S2b, docs/h1_s2b_hex_ownership.md). Like
# host_match it rides CAP_OBJS, so it reaches the platform lib (base + cosmo),
# the hull binary, libhull, and every TEST_CAP_OBJS suite with one edit. Harmless
# where unreferenced. Consumers (signature/release/sbom/blob_store/db_postgres/
# verify_self/mod_tool) include it by relative path; it is not public API.
HEX_OBJ := $(BUILDDIR)/hex.o
CAP_OBJS += $(HEX_OBJ)
CAP_TOOL_OBJ := $(BUILDDIR)/cap_tool.o
# cap/test.c is the in-process HTTP test harness - depends on KlRouter
# and the rest of Keel's request/response machinery. Server-only.
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
CAP_TEST_OBJ :=
else
CAP_TEST_OBJ := $(BUILDDIR)/cap_test.o
endif

# JS runtime sources
JS_RT_SRCS := $(wildcard $(SRCDIR)/hull/runtime/js/*.c)
ifeq ($(HL_ENABLE_IMAGE),0)
  JS_RT_SRCS := $(filter-out $(SRCDIR)/hull/runtime/js/mod_image.c,$(JS_RT_SRCS))
endif
ifeq ($(HL_ENABLE_DB),0)
  JS_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/js/mod_db.c \
      $(SRCDIR)/hull/runtime/js/mod_db_udf.c \
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
  # mod_test depends on hl_cap_test_dispatch which is in cap/test.c -
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
      $(SRCDIR)/hull/runtime/js/mod_request.c \
      $(SRCDIR)/hull/runtime/js/bindings.c \
      $(SRCDIR)/hull/runtime/js/bindings_response.c, \
      $(JS_RT_SRCS))
endif
ifeq ($(HL_ENABLE_TUI),0)
  # The tui native bridge moves to libhull_feature-tui.a (composed feature) -
  # drop it from the base (which keeps only the weak feature hooks in
  # cap/tui_feature.c). Pairs with the cap/tui.c + tui_input.c filter above.
  JS_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/js/mod_tui.c, \
      $(JS_RT_SRCS))
endif
JS_RT_OBJS := $(patsubst $(SRCDIR)/hull/runtime/js/%.c,$(BUILDDIR)/js_%.o,$(JS_RT_SRCS))

# Lua runtime sources
LUA_RT_SRCS := $(wildcard $(SRCDIR)/hull/runtime/lua/*.c)
ifeq ($(HL_ENABLE_IMAGE),0)
  LUA_RT_SRCS := $(filter-out $(SRCDIR)/hull/runtime/lua/mod_image.c,$(LUA_RT_SRCS))
endif
ifeq ($(HL_ENABLE_DB),0)
  LUA_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/lua/mod_db.c \
      $(SRCDIR)/hull/runtime/lua/mod_db_udf.c \
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
      $(SRCDIR)/hull/runtime/lua/mod_request.c \
      $(SRCDIR)/hull/runtime/lua/bindings.c \
      $(SRCDIR)/hull/runtime/lua/bindings_response.c, \
      $(LUA_RT_SRCS))
endif
ifeq ($(HL_ENABLE_TUI),0)
  # The tui native bridge moves to libhull_feature-tui.a (composed feature) -
  # drop it from the base, which carries only the weak feature hooks
  # (cap/tui_feature.c). Pairs with the cap/tui.c + tui_input.c filter above.
  LUA_RT_SRCS := $(filter-out \
      $(SRCDIR)/hull/runtime/lua/mod_tui.c, \
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
# unconditionally - every runtime cache (Lua + JS, bytecode + template)
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
SANDBOX_TOOL_OBJ := $(BUILDDIR)/sandbox_tool.o
EMBED_OBJ        := $(BUILDDIR)/embed.o

# Async backend implementations
#   async/keel.c - wraps Keel's KlEventCtx + KlThreadPool. Compiled
#                  whenever Keel is linked (either HTTP half on).
#   async/poll.c - freestanding poll(2) + pthread impl. Always built;
#                  selected by hl_async_backend() when neither HTTP
#                  half is compiled in.
ASYNC_BACKEND_SRCS := $(wildcard $(SRCDIR)/hull/async/*.c)
# Drop the Keel event-loop backend (async/keel.c) from the base when HTTP is off
# OR when building the Keel-less app-build base (HL_KEEL_FEATURE=1):
# the base keeps only the weak poll backend, and async/keel.c (the strong
# hl_async_backend override) composes back in the whole-archived http feature.
ifneq ($(filter 1,$(if $(filter 0,$(HL_ENABLE_HTTP_ANY)),1,) $(HL_KEEL_FEATURE)),)
  ASYNC_BACKEND_SRCS := $(filter-out $(SRCDIR)/hull/async/keel.c,$(ASYNC_BACKEND_SRCS))
endif
ASYNC_BACKEND_OBJS := $(patsubst $(SRCDIR)/hull/async/%.c,$(BUILDDIR)/async_%.o,$(ASYNC_BACKEND_SRCS))

# Net backend implementations
#   net/keel.c - Keel-backed HlNetBackend (op_suspend / op_complete
#                pair). Server-only: the only callers are server-side
#                connection-bound request suspension. CLIENT-only or
#                pure-compute builds use the no-op stubs in async/poll.c.
NET_BACKEND_SRCS := $(wildcard $(SRCDIR)/hull/net/*.c)
# Drop the Keel net backend (net/keel.c) from the base when the server is off OR
# when building the Keel-less app-build base (HL_KEEL_FEATURE=1): net/keel.c
# references kl_async_* and would re-pull Keel via shared/async.o. The weak no-op
# stubs in async/poll.c stand in the base; net/keel.c composes back (strong) in
# the whole-archived http feature on needs_http.
ifneq ($(filter 1,$(if $(filter 0,$(HL_ENABLE_HTTP_SERVER)),1,) $(HL_KEEL_FEATURE)),)
  NET_BACKEND_SRCS :=
endif
NET_BACKEND_OBJS := $(patsubst $(SRCDIR)/hull/net/%.c,$(BUILDDIR)/net_%.o,$(NET_BACKEND_SRCS))

# Test-specific objects (single runtime - avoids pulling Lua into JS tests
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
# release_io.o holds the HTTPS fetch path (hull update / tools install) AND
# the offline release helpers (platform, self_path, json_str, sha256_hex,
# find_checksum, atomic_write). The HTTPS half is #ifdef HL_ENABLE_HTTP_CLIENT
# inside the TU; the offline half is always needed (verify-self + the
# platform-signature verifier), so the object is always built and linked.
RELEASE_IO_OBJ := $(BUILDDIR)/release_io.o
# tools_install.o is always linked - `hl_tools_lookup_path` is used by
# cap/wasm.c for wamrc resolution even on HL_ENABLE_HTTP_CLIENT=0 builds.
TOOLS_INSTALL_OBJ := $(BUILDDIR)/tools_install.o
# Platform manifest builder + signer + verifier + per-arch extractor.
# Pure functions; reuses release.c (sign/verify) + release_io.c
# (find_checksum). Always built - verify path uses it on every signed
# app, regardless of HL_ENABLE_HTTP_CLIENT.
PLATFORM_SIG_OBJ := $(BUILDDIR)/platform_sig.o
# Accessor for the embedded signed platform-sig blob. CI's
# sign-platform-manifest job emits the header it includes; local
# builds get a placeholder that signals "no embedded blob" via the
# accessor's -1 return. NOT in PLATFORM_OBJS - apps don't need the
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
# test_runner.c uses KlRouter to dispatch in-process test requests -
# server-only.
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
TEST_RUNNER_OBJ :=
else
TEST_RUNNER_OBJ := $(BUILDDIR)/test_runner.o
endif
RUNTIME_FACTORY_OBJ := $(BUILDDIR)/runtime_factory.o
# Explicit empty default for the composable runtime-factory seam. The base ships
# NO weak default for hl_runtime_feature_factories() (see runtime/factory.c), so
# a forgotten override is a link-time error, not a silent empty-runtime boot.
# Non-app link targets that compose no runtime (the unit-test binaries, which
# init runtimes directly) link this explicit empty. (The sibling stdlib seam
# hl_stdlib_feature_entries() has no such stub: every consumer today links a real
# registry - the toolchain one or a produced app's emitted one.)
RUNTIME_FACTORY_NONE_OBJ := $(BUILDDIR)/runtime_factory_none.o
# (RUNTIME_CACHE_COMMON_OBJ is defined earlier - see the runtime
# selection block - because RT_OBJS references it.)
# static.c serves embedded static files via Keel response writers -
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
STDLIB_FEATURE_OBJ := $(BUILDDIR)/stdlib_feature.o
PATH_NORM_OBJ  := $(BUILDDIR)/path_normalize.o
THREAD_AFFINITY_OBJ := $(BUILDDIR)/thread_affinity.o
LOG_LOCK_OBJ   := $(BUILDDIR)/log_lock.o
# Low-level content-addressed blob store. Shared between cap/blob.c
# (manifest-gated app capability) and runtime infrastructure (Lua
# bytecode cache, compute AOT cache, future template cache). Apps
# never see this layer directly - they go through hl_cap_blob_*.
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
# Shared directory-creation helpers (hl_ensure_dir / hl_mkdir_p). Zero-dependency
# (pure libc); the single home for what used to be three duplicate mkdir_p copies
# in blob_store.c, cache_dir.c, and tools_install.c. Also used by cap/tar.c.
FS_UTIL_OBJ    := $(BUILDDIR)/fs_util.o
CACERT_OBJ     := $(BUILDDIR)/cacert.o
# Blocking TLS client helper over Keel's KlTls (handshake + read/write),
# shared by protocols that upgrade a plaintext socket on demand (today:
# PostgreSQL SSLRequest; a retrofitted SMTP STARTTLS later). Only built
# when TLS is linked at all (an HTTP half or PostgreSQL enabled).
ifeq ($(HL_LINK_TLS),1)
ifeq ($(HL_TLS_FEATURE),1)
TLS_CLIENT_OBJ :=   # -> libhull_feature-tls.a
else
TLS_CLIENT_OBJ := $(BUILDDIR)/tls_client.o
endif
else
TLS_CLIENT_OBJ :=
endif

# TLS transport seam (docs/tls_feature.md, a1). serve.c / serve_cli.c set up
# Keel's server + client TLS contexts through the weak hl_tls_* accessors instead
# of calling kl_tls_mbedtls_* directly. The STRONG override (tls_transport.o) is
# the sole in-Hull consumer of Keel's tls_mbedtls.o for ctx setup; built only when
# TLS is linked, and a later phase moves it into libhull_feature-tls.a so a
# TLS-less base drops it. The weak stub (tls_transport_stub.o) is always in the
# base so serve.c links when the override is absent (serves plaintext HTTP).
ifeq ($(HL_LINK_TLS),1)
ifeq ($(HL_TLS_FEATURE),1)
TLS_TRANSPORT_OBJ :=   # -> libhull_feature-tls.a (the strong override)
else
TLS_TRANSPORT_OBJ := $(BUILDDIR)/tls_transport.o
endif
else
TLS_TRANSPORT_OBJ :=
endif
TLS_TRANSPORT_STUB_OBJ := $(BUILDDIR)/tls_transport_stub.o

# Content-Security-Policy preset registry. Tiny (~70 LOC, no deps
# beyond <string.h>) so the test surface stays cheap. Resolves
# `app.manifest({csp = "<name>"})` to a concrete header value at
# startup; unknown names pass through as literal CSP strings.
CSP_OBJ        := $(BUILDDIR)/csp.o
# Page-backed bump arena with one-way mprotect-to-RO seal. For
# manifest-derived security policy that's boot-built then read-only
# at runtime - the dispatch / vtable / registry tables are already
# `static const`, so this targets the OTHER category.
#
# Source lives in Hull's own vendored copy at vendor/sh_seal_arena/
# (extracted from Keel v2.7.1; Keel v3 no longer ships it). We compile
# it with Hull's CFLAGS (including sanitizers under DEBUG/MSAN). Keel v3
# provides no sh_seal_arena_* symbols, so this object is the ONLY
# definition -- it is linked into the hull binary, the platform archive
# (so composed `hull build` apps resolve it), and test binaries.
# Required for MSan: without an instrumented sh_seal_arena.o, MSan
# can't see init writing to ShSealArena fields and flags every
# post-init read as use-of-uninitialized-value.
SH_SEAL_ARENA_OBJ := $(BUILDDIR)/sh_seal_arena.o
SBOM_OBJ       := $(BUILDDIR)/sbom.o
APP_CONTEXT_OBJ := $(BUILDDIR)/app_context.o
APP_CONTEXT_RT_OBJ := $(BUILDDIR)/app_context_runtime.o
AGENT_LIB_SRCS := $(wildcard $(SRCDIR)/hull/agent/*.c)
ifeq ($(HL_ENABLE_DB),0)
  AGENT_LIB_SRCS := $(filter-out $(SRCDIR)/hull/agent/db.c,$(AGENT_LIB_SRCS))
endif
ifneq ($(HL_ENABLE_SQLITE),1)
  # SQLite-only agent introspection (raw sqlite3 API): drop from the base and
  # ship in libhull_feature-sqlite.a (docs/sqlite_feature.md). The base
  # keeps agent/db_stub.c's weak entry points (A.2). Covers the postgres/mysql
  # base builds too, where these compiled to empty TUs before.
  AGENT_LIB_SRCS := $(filter-out \
      $(SRCDIR)/hull/agent/db.c \
      $(SRCDIR)/hull/agent/sql.c \
      $(SRCDIR)/hull/agent/schema_diff.c \
      $(SRCDIR)/hull/agent/db_open.c, \
      $(AGENT_LIB_SRCS))
endif
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
  # agent/test.c calls hl_test_runner_run + the in-process HTTP harness;
  # agent/request.c, agent/eval.c, agent/perf.c, agent/endpoint.c also
  # exercise HTTP routes. All server-only - the `hull agent` subcommands
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
# introspection) drops out too - it speaks HTTP to a running server.
ifeq ($(HL_ENABLE_HTTP_SERVER),0)
AGENT_API_OBJ  :=
SERVE_OBJ      := $(BUILDDIR)/serve_cli.o
else
AGENT_API_OBJ  := $(BUILDDIR)/agent_api.o
SERVE_OBJ      := $(BUILDDIR)/serve.o
endif
# HL_KEEL_FEATURE=1 (docs/keel_feature.md): the Keel-less app-build
# base. Even at HTTP_SERVER=1 it uses the Keel-free serve_cli.o app-entry (weak
# hull_serve; compiles clean under HTTP_SERVER=1 via the a1/4.1/4.2a seams)
# instead of serve.o (the KlServer loop, strong hull_serve), which composes back
# in the whole-archived http feature on needs_http. agent_api (in-process HTTP
# introspection) drops out with the server.
ifeq ($(HL_KEEL_FEATURE),1)
AGENT_API_OBJ  :=
SERVE_OBJ      := $(BUILDDIR)/serve_cli.o
endif
MAIN_OBJ       := $(BUILDDIR)/main.o
APP_RUNNER_OBJ := $(BUILDDIR)/app_runner.o
ENTRY_OBJ      := $(BUILDDIR)/entry.o
# Weak no-op defaults for the per-runtime web bindings (issue #114).
# The web bindings live in libhull_feature-http-<rt>.a; the runtime-less base
# carries these weak stubs so an HTTP-free composed app links (the strong defs
# override when the web archive is whole-archived). Compiles to an empty TU when
# HTTP_SERVER is off (the whole body is guarded).
HTTP_WEAKSTUB_OBJ := $(BUILDDIR)/http_weakstub.o

# WASM-as-a-feature seam (docs/wasm_feature.md). Weak, fail-closed
# defaults for the eleven runtime-agnostic wasm cap symbols base objects
# (db_udf / mod_buffer / mod_image / mod_gpu / app_context / serve) reference, so
# a future compute-less base still links. Additive + dormant today: the strong
# cap_wasm* defs win while WAMR is still compiled in.
WASM_WEAKSTUB_OBJ := $(BUILDDIR)/wasm_weakstub.o

# IMAGE-as-a-feature seam (docs/image_feature.md). Weak, fail-closed defaults for
# the two runtime-agnostic image cap symbols base objects reference (mod_gpu:
# gpu.texture_read builds an HlImage, the paired free), so an image-less base
# links. The strong cap_image defs win when the image feature is composed (or on
# a cosmo full base). Mirrors WASM_WEAKSTUB_OBJ.
IMAGE_WEAKSTUB_OBJ := $(BUILDDIR)/image_weakstub.o

# ── Stdlib embedding (xxd) ──────────────────────────────────────────
#
# Two Lua source trees feed the embedded stdlib registry:
#
#   stdlib/lua/hull/*.lua       - user-facing modules apps may
#                                 require("hull.foo"): template, jwt,
#                                 cookie, csrf, csv, email, form, i18n,
#                                 json, search, validate, plus
#                                 middleware/*.
#
#   stdlib/cli/lua/hull/*.lua   - CLI plugins invoked only by the C
#                                 dispatcher (`hull build`, `hull deploy`,
#                                 etc.) via hull_tool(); never imported
#                                 by app code. Split out per audit A-2
#                                 so stdlib/lua/hull/ honestly reflects
#                                 the user-facing surface.
#
# Both trees go through the same xxd pipeline and end up in
# hl_stdlib_entries[]. The name-strip rule below makes
# stdlib/cli/lua/hull/build.lua resolve as "hull.build" - same name
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
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
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
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(STDLIB_JS_FILES),$(eval $(call STDLIB_JS_RULE,$(f))))

STDLIB_JS_XXD_HDRS := $(STDLIB_JS_HDRS)

# ── cli-js tooling embedding (the restricted-QuickJS JS source frontend) ──────
# stdlib/cli/js/** are TRUSTED, tooling-only JS modules loaded ONLY into the restricted
# QuickJS tooling runtime (src/hull/frontend/js_session.c) -- never the application JS
# runtime. Names strip stdlib/cli/js/: stdlib/cli/js/hull/probe.js -> hull:probe.
STDLIB_JS_CLI_FILES := $(shell find stdlib/cli/js -name '*.js' -not -path '*/tests/*' 2>/dev/null)
stdlib_js_cli_hdr = $(BUILDDIR)/$(subst /,_,$(patsubst stdlib/%.js,stdlib_%.h,$(1)))
STDLIB_JS_CLI_HDRS := $(foreach f,$(STDLIB_JS_CLI_FILES),$(call stdlib_js_cli_hdr,$(f)))
define STDLIB_JS_CLI_RULE
$(call stdlib_js_cli_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(STDLIB_JS_CLI_FILES),$(eval $(call STDLIB_JS_CLI_RULE,$(f))))

STDLIB_JS_CLI_REGISTRY_C := $(BUILDDIR)/stdlib_js_cli_registry.c
STDLIB_JS_CLI_REGISTRY_O := $(BUILDDIR)/stdlib_js_cli_registry.o
$(STDLIB_JS_CLI_REGISTRY_C): $(STDLIB_JS_CLI_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated cli-js tooling registry - do not edit */" > $@
	@( for hdr in $(STDLIB_JS_CLI_HDRS); do echo "#include \"$$(basename $$hdr)\""; done ) | LC_ALL=C sort >> $@
	@echo "" >> $@
	@echo "#include \"hull/entry.h\"" >> $@
	@echo "const HlEntry hl_stdlib_js_cli_entries[] = {" >> $@
	@( for f in $(STDLIB_JS_CLI_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.\-]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/cli/js/||; s|\.js$$||; s|/|:|g'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done ) | LC_ALL=C sort | cut -f2- >> $@
	@echo "    { 0, 0, 0 }" >> $@
	@echo "};" >> $@
$(STDLIB_JS_CLI_REGISTRY_O): $(STDLIB_JS_CLI_REGISTRY_C) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# TEST-ONLY cli-js registry: the production files PLUS the */tests/* tooling modules (the
# authority probe). Same array symbol as the production registry, so a test that links THIS
# object -- NEVER both -- sees the extra module while the shipped registry stays free of it.
# Only test_js_generation links it (see mk/tests.mk). This is what keeps hull:source:tests:*
# absent from every shipped binary while remaining reachable to the manager authority test.
STDLIB_JS_CLI_TEST_ONLY_FILES := $(shell find stdlib/cli/js -name '*.js' -path '*/tests/*' 2>/dev/null)
STDLIB_JS_CLI_TEST_ONLY_HDRS := $(foreach f,$(STDLIB_JS_CLI_TEST_ONLY_FILES),$(call stdlib_js_cli_hdr,$(f)))
$(foreach f,$(STDLIB_JS_CLI_TEST_ONLY_FILES),$(eval $(call STDLIB_JS_CLI_RULE,$(f))))
STDLIB_JS_CLI_TEST_FILES := $(STDLIB_JS_CLI_FILES) $(STDLIB_JS_CLI_TEST_ONLY_FILES)

STDLIB_JS_CLI_TEST_REGISTRY_C := $(BUILDDIR)/stdlib_js_cli_test_registry.c
STDLIB_JS_CLI_TEST_REGISTRY_O := $(BUILDDIR)/stdlib_js_cli_test_registry.o
$(STDLIB_JS_CLI_TEST_REGISTRY_C): $(STDLIB_JS_CLI_HDRS) $(STDLIB_JS_CLI_TEST_ONLY_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated TEST cli-js tooling registry - do not edit */" > $@
	@( for hdr in $(STDLIB_JS_CLI_HDRS) $(STDLIB_JS_CLI_TEST_ONLY_HDRS); do echo "#include \"$$(basename $$hdr)\""; done ) | LC_ALL=C sort >> $@
	@echo "" >> $@
	@echo "#include \"hull/entry.h\"" >> $@
	@echo "const HlEntry hl_stdlib_js_cli_entries[] = {" >> $@
	@( for f in $(STDLIB_JS_CLI_TEST_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.\-]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/cli/js/||; s|\.js$$||; s|/|:|g'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done ) | LC_ALL=C sort | cut -f2- >> $@
	@echo "    { 0, 0, 0 }" >> $@
	@echo "};" >> $@
$(STDLIB_JS_CLI_TEST_REGISTRY_O): $(STDLIB_JS_CLI_TEST_REGISTRY_C) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# The restricted QuickJS tooling runtime (needs QuickJS; lives in the hull binary only).
FRONTEND_JS_SESSION_OBJ := $(BUILDDIR)/frontend_js_session.o
$(FRONTEND_JS_SESSION_OBJ): $(SRCDIR)/hull/frontend/js_session.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Ivendor/quickjs -c -o $@ $<

# The JS source-frontend generation/session manager (wraps js_session; C-owned lifetime +
# monotonic tokens). Linked into hull only when QuickJS is linked (NOT a lua-only build).
FRONTEND_JS_GEN_OBJ := $(BUILDDIR)/frontend_js_generation.o
$(FRONTEND_JS_GEN_OBJ): $(SRCDIR)/hull/frontend/js_generation.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Ivendor/quickjs -c -o $@ $<

# ── Context doc embedding (xxd) ───────────────────────────────────────
#
# Markdown docs in stdlib/context/*.md are embedded for hull agent context.
# Names use context: prefix: stdlib/context/auth.md → context:auth

CONTEXT_FILES := $(wildcard stdlib/context/*.md)

context_hdr = $(BUILDDIR)/$(subst /,_,$(patsubst stdlib/context/%.md,context_%.h,$(1)))
CONTEXT_HDRS := $(foreach f,$(CONTEXT_FILES),$(call context_hdr,$(f)))

define CONTEXT_RULE
$(call context_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
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
# Ships no widget assets - discovery is wired and tested but
# returns 0 entries until §1.5.g-1 lands.

STDLIB_STATIC_FILES := $(shell find stdlib/static -type f \
    -not -name '.gitkeep' 2>/dev/null)

# Path flatten: stdlib/static/hull/htmx/toast/toast.css ->
#               build/stdlib_static_hull_htmx_toast_toast.css.h
stdlib_static_hdr = $(BUILDDIR)/stdlib_static_$(subst /,_,$(patsubst stdlib/static/%,%.h,$(1)))
STDLIB_STATIC_HDRS := $(foreach f,$(STDLIB_STATIC_FILES),$(call stdlib_static_hdr,$(f)))

define STDLIB_STATIC_RULE
$(call stdlib_static_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(STDLIB_STATIC_FILES),$(eval $(call STDLIB_STATIC_RULE,$(f))))

STDLIB_STATIC_XXD_HDRS := $(STDLIB_STATIC_HDRS)

# ── Stdlib-shipped template partials (xxd) ────────────────────────────
#
# Files under stdlib/templates/hull/<module>/*.html become embedded
# entries named `templates/hull/<module>/<file>`. The template engine
# (stdlib/lua/hull/template.lua + JS sibling) falls back to the
# platform VFS after an app-VFS miss; app-side templates at the same
# path win. Ships no widget templates.

STDLIB_TPL_FILES := $(shell find stdlib/templates -name '*.html' \
    -not -name '.gitkeep' 2>/dev/null)

# Path flatten: stdlib/templates/hull/htmx/toast/toast.html ->
#               build/stdlib_tpl_hull_htmx_toast_toast.h
stdlib_tpl_hdr = $(BUILDDIR)/stdlib_tpl_$(subst /,_,$(patsubst stdlib/templates/%.html,%.h,$(1)))
STDLIB_TPL_HDRS := $(foreach f,$(STDLIB_TPL_FILES),$(call stdlib_tpl_hdr,$(f)))

define STDLIB_TPL_RULE
$(call stdlib_tpl_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(STDLIB_TPL_FILES),$(eval $(call STDLIB_TPL_RULE,$(f))))

STDLIB_TPL_XXD_HDRS := $(STDLIB_TPL_HDRS)

# ── Stdlib registries: base (runtime-agnostic) + per-runtime halves ──
#
# The base registry (hl_stdlib_entries[]) holds only runtime-agnostic entries:
# context docs, static assets, templates. Each runtime's stdlib modules live in
# their own array (hl_stdlib_lua_entries[] / _js_entries[]) so they can travel
# into that runtime's feature archive. A generated toolchain registry fills the
# weak hl_stdlib_feature_entries() hook with whichever runtime arrays are
# compiled in; the platform VFS unions the base with them at init
# (hl_vfs_init_composed). Runtimes still filter a merged name at load time:
# strchr(name, ':') -> JS, else Lua.

STDLIB_REGISTRY_C := $(BUILDDIR)/stdlib_registry.c
STDLIB_REGISTRY_O := $(BUILDDIR)/stdlib_registry.o
STDLIB_LUA_REGISTRY_C := $(BUILDDIR)/stdlib_lua_registry.c
STDLIB_LUA_REGISTRY_O := $(BUILDDIR)/stdlib_lua_registry.o
STDLIB_JS_REGISTRY_C := $(BUILDDIR)/stdlib_js_registry.c
STDLIB_JS_REGISTRY_O := $(BUILDDIR)/stdlib_js_registry.o
STDLIB_TOOLCHAIN_REGISTRY_C := $(BUILDDIR)/stdlib_toolchain_registry.c
STDLIB_TOOLCHAIN_REGISTRY_O := $(BUILDDIR)/stdlib_toolchain_registry.o
RUNTIME_TOOLCHAIN_REGISTRY_C := $(BUILDDIR)/runtime_toolchain_registry.c
RUNTIME_TOOLCHAIN_REGISTRY_O := $(BUILDDIR)/runtime_toolchain_registry.o

# Which per-runtime stdlib arrays this base compiles in, and the symbols the
# toolchain registry exposes through the two feature hooks (default = both):
# STDLIB_FEATURE_SYMS -> hl_stdlib_feature_entries (the runtime's stdlib VFS
# array), RUNTIME_FACTORY_SYMS -> hl_runtime_feature_factories (its factory
# descriptor). The base g_factories is empty, so the toolchain registry is what
# makes `hull` resolve both runtimes; a produced app gets the same two hooks
# (for its one runtime) from build.lua's generated registry instead.
ifeq ($(RUNTIME),js)
  STDLIB_RT_REGISTRY_OBJS := $(STDLIB_JS_REGISTRY_O)
  STDLIB_FEATURE_SYMS     := hl_stdlib_js_entries
  RUNTIME_FACTORY_SYMS    := hl_js_factory
else ifeq ($(RUNTIME),lua)
  STDLIB_RT_REGISTRY_OBJS := $(STDLIB_LUA_REGISTRY_O)
  STDLIB_FEATURE_SYMS     := hl_stdlib_lua_entries
  RUNTIME_FACTORY_SYMS    := hl_lua_factory
else
  STDLIB_RT_REGISTRY_OBJS := $(STDLIB_LUA_REGISTRY_O) $(STDLIB_JS_REGISTRY_O)
  STDLIB_FEATURE_SYMS     := hl_stdlib_lua_entries hl_stdlib_js_entries
  RUNTIME_FACTORY_SYMS    := hl_lua_factory hl_js_factory
endif

$(STDLIB_REGISTRY_C): $(CONTEXT_XXD_HDRS) $(STDLIB_STATIC_XXD_HDRS) $(STDLIB_TPL_XXD_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated stdlib registry - do not edit */" > $@
	@( for hdr in $(CONTEXT_XXD_HDRS) $(STDLIB_STATIC_XXD_HDRS) $(STDLIB_TPL_XXD_HDRS); do \
		echo "#include \"$$(basename $$hdr)\""; \
	done ) | LC_ALL=C sort >> $@
	@echo "" >> $@
	@echo "#include \"hull/entry.h\"" >> $@
	@echo "const HlEntry hl_stdlib_entries[] = {" >> $@
	@( for f in $(CONTEXT_FILES); do \
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

$(STDLIB_LUA_REGISTRY_C): $(STDLIB_LUA_XXD_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated stdlib registry - do not edit */" > $@
	@( for hdr in $(STDLIB_LUA_XXD_HDRS); do \
		echo "#include \"$$(basename $$hdr)\""; \
	done ) | LC_ALL=C sort >> $@
	@echo "" >> $@
	@echo "#include \"hull/entry.h\"" >> $@
	@echo "const HlEntry hl_stdlib_lua_entries[] = {" >> $@
	@( for f in $(STDLIB_LUA_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.\-]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/lua/||; s|^stdlib/cli/lua/||; s|\.lua$$||; s|/|.|g'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done ) | LC_ALL=C sort | cut -f2- >> $@
	@echo "    { 0, 0, 0 }" >> $@
	@echo "};" >> $@

$(STDLIB_JS_REGISTRY_C): $(STDLIB_JS_XXD_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated stdlib registry - do not edit */" > $@
	@( for hdr in $(STDLIB_JS_XXD_HDRS); do \
		echo "#include \"$$(basename $$hdr)\""; \
	done ) | LC_ALL=C sort >> $@
	@echo "" >> $@
	@echo "#include \"hull/entry.h\"" >> $@
	@echo "const HlEntry hl_stdlib_js_entries[] = {" >> $@
	@( for f in $(STDLIB_JS_FILES); do \
		varname=$$(echo "$$f" | sed 's/[\/.\-]/_/g'); \
		modname=$$(echo "$$f" | sed 's|^stdlib/js/||; s|\.js$$||; s|/|:|g'); \
		echo "$$modname	    { \"$$modname\", $${varname}, sizeof($${varname}) },"; \
	done ) | LC_ALL=C sort | cut -f2- >> $@
	@echo "    { 0, 0, 0 }" >> $@
	@echo "};" >> $@

# Toolchain STDLIB registry: STRONG hl_stdlib_feature_entries() over the runtime
# stdlib arrays compiled into this base. Linked into hull AND the test binaries
# (which build a platform VFS but init runtimes directly). Both stdlib arrays
# are always present where this links, so it references only HlEntry symbols.
# Depends on Makefile: emitted from $(STDLIB_FEATURE_SYMS) (and the recipe),
# which live here, not in a tracked source file - else the .c never regenerates.
$(STDLIB_TOOLCHAIN_REGISTRY_C): Makefile | $(BUILDDIR)
	@echo "/* Auto-generated toolchain stdlib registry - do not edit */" > $@
	@echo "#include <stddef.h>" >> $@
	@echo "#include \"hull/entry.h\"" >> $@
	@for s in $(STDLIB_FEATURE_SYMS); do echo "extern const HlEntry $$s[];" >> $@; done
	@echo "static const HlEntry *const HL_STDLIB_FEATS[] = {" >> $@
	@for s in $(STDLIB_FEATURE_SYMS); do echo "    $$s," >> $@; done
	@echo "};" >> $@
	@echo "const HlEntry *const *hl_stdlib_feature_entries(size_t *count) {" >> $@
	@echo "    if (count) *count = sizeof(HL_STDLIB_FEATS)/sizeof(HL_STDLIB_FEATS[0]);" >> $@
	@echo "    return HL_STDLIB_FEATS;" >> $@
	@echo "}" >> $@

# Toolchain RUNTIME-FACTORY registry: STRONG hl_runtime_feature_factories() over
# the factory descriptors. This references hl_<rt>_factory, so it links ONLY into
# `hull` (which has every runtime). A JS-only test binary must NOT link it (it
# has no hl_lua_factory) - and doesn't need it, since tests init runtimes
# directly. A produced app gets its own one-runtime version from build.lua.
$(RUNTIME_TOOLCHAIN_REGISTRY_C): Makefile | $(BUILDDIR)
	@echo "/* Auto-generated toolchain runtime-factory registry - do not edit */" > $@
	@echo "#include <stddef.h>" >> $@
	@echo "typedef struct HlRuntimeFactory HlRuntimeFactory;" >> $@
	@for s in $(RUNTIME_FACTORY_SYMS); do echo "extern const HlRuntimeFactory $$s;" >> $@; done
	@echo "static const HlRuntimeFactory *const HL_RT_FEATS[] = {" >> $@
	@for s in $(RUNTIME_FACTORY_SYMS); do echo "    &$$s," >> $@; done
	@echo "};" >> $@
	@echo "const HlRuntimeFactory *const *hl_runtime_feature_factories(size_t *count) {" >> $@
	@echo "    if (count) *count = sizeof(HL_RT_FEATS)/sizeof(HL_RT_FEATS[0]);" >> $@
	@echo "    return HL_RT_FEATS;" >> $@
	@echo "}" >> $@

$(STDLIB_REGISTRY_O): $(STDLIB_REGISTRY_C) | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -I$(INCDIR) -I$(BUILDDIR) -c -o $@ $<
$(STDLIB_LUA_REGISTRY_O): $(STDLIB_LUA_REGISTRY_C) | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -I$(INCDIR) -I$(BUILDDIR) -c -o $@ $<
$(STDLIB_JS_REGISTRY_O): $(STDLIB_JS_REGISTRY_C) | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -I$(INCDIR) -I$(BUILDDIR) -c -o $@ $<
$(STDLIB_TOOLCHAIN_REGISTRY_O): $(STDLIB_TOOLCHAIN_REGISTRY_C) | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -I$(INCDIR) -I$(BUILDDIR) -c -o $@ $<
$(RUNTIME_TOOLCHAIN_REGISTRY_O): $(RUNTIME_TOOLCHAIN_REGISTRY_C) | $(BUILDDIR)
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
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(APP_LUA_FILES),$(eval $(call APP_LUA_RULE,$(f))))

define APP_JS_RULE
$(call app_js_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(APP_JS_FILES),$(eval $(call APP_JS_RULE,$(f))))

define APP_JSON_RULE
$(call app_json_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(APP_JSON_FILES),$(eval $(call APP_JSON_RULE,$(f))))

define APP_TPL_RULE
$(call app_tpl_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(APP_TPL_FILES),$(eval $(call APP_TPL_RULE,$(f))))

define APP_STATIC_RULE
$(call app_static_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(APP_STATIC_FILES),$(eval $(call APP_STATIC_RULE,$(f))))

define APP_MIGRATION_RULE
$(call app_migration_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(APP_MIGRATION_FILES),$(eval $(call APP_MIGRATION_RULE,$(f))))

define APP_COMPUTE_RULE
$(call app_compute_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(APP_COMPUTE_FILES),$(eval $(call APP_COMPUTE_RULE,$(f))))

define APP_SHADER_RULE
$(call app_shader_hdr,$(1)): $(1) | $(BUILDDIR)
	xxd -i $$< > $$@ && $(XXD_CONST_SEAL) $$@ && rm -f $$@.bak
endef
$(foreach f,$(APP_SHADER_FILES),$(eval $(call APP_SHADER_RULE,$(f))))

APP_ALL_XXD_HDRS := $(APP_LUA_HDRS) $(APP_JS_HDRS) $(APP_JSON_HDRS) $(APP_TPL_HDRS) $(APP_STATIC_HDRS) $(APP_MIGRATION_HDRS) $(APP_COMPUTE_HDRS) $(APP_SHADER_HDRS)

APP_REGISTRY_C := $(BUILDDIR)/app_registry.c
APP_REGISTRY_O := $(BUILDDIR)/app_registry.o

$(APP_REGISTRY_C): $(APP_ALL_XXD_HDRS) | $(BUILDDIR)
	@echo "/* Auto-generated unified app registry - do not edit */" > $@
	@# Sort the #include emission so embedded app-file data lands in .rodata in a
	@# deterministic order (same reproducibility rationale as stdlib_registry.c).
	@( for hdr in $(APP_ALL_XXD_HDRS); do \
		echo "#include \"$$(basename $$hdr)\""; \
	done ) | LC_ALL=C sort >> $@
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

# App entries default (empty array - used when no APP_DIR)
$(APP_ENTRIES_DEFAULT_OBJ): $(SRCDIR)/hull/app_entries_default.c $(INCDIR)/hull/entry.h | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -I$(INCDIR) -c -o $@ $<

# ── Include paths ───────────────────────────────────────────────────

INCLUDES := -I$(INCDIR) -I$(QJS_DIR) -I$(LUA_DIR) -I$(KEEL_INC) -I$(KEEL_DIR)/integrations/tls/mbedtls -I$(KEEL_DIR)/integrations/codec/miniz -I$(KEEL_DIR)/vendor/llhttp -I$(VENDDIR)/sh_seal_arena -I$(MBEDTLS_DIR)/include -I$(SQLITE_DIR) -I$(LOG_DIR) -I$(SH_ARENA_DIR) -I$(SH_JSON_DIR) -I$(TWEETNACL_DIR) -I$(STB_DIR) -I$(VENDDIR) -I$(BUILDDIR) $(WAMR_INC)

# ── Build-flag fingerprint (force-rebuild on flag change) ───────────
#
# Make tracks file mtimes; it doesn't notice when `-D` defines
# change between invocations. Without this, switching between e.g.
# `make` and `make HL_ENABLE_HTTP=0` reuses .o files compiled with
# the wrong defines - manifests as duplicate-symbol link errors
# (poll.c's stubs collide with net/keel.c's real impls), wrong
# code paths active, or stale conditional logic.
#
# Mechanism: at parse time, compute a fingerprint string of every
# flag that flows into CFLAGS. If it differs from the previous run,
# delete every Hull-owned .o (and the binaries that link them) so
# make naturally rebuilds them from source with the new flags.
# Vendor .o files (mbedTLS, WAMR, QuickJS, Lua, SQLite) are kept -
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
  SQLITE=$(HL_ENABLE_SQLITE)|\
  POSTGRES=$(HL_ENABLE_POSTGRES)|\
  MYSQL=$(HL_ENABLE_MYSQL)|\
  VALKEY=$(HL_ENABLE_VALKEY)|\
  WASM=$(HL_ENABLE_WASM)|\
  GPU=$(HL_ENABLE_GPU)|\
  TUI=$(HL_ENABLE_TUI)|\
  TUI_TC=$(HL_TUI_TOOLCHAIN)|\
  IMAGE=$(HL_ENABLE_IMAGE)|\
  TLS_FEATURE=$(HL_TLS_FEATURE)|\
  KEEL_FEATURE=$(HL_KEEL_FEATURE)|\
  CA=$(HL_EMBED_CA_BUNDLE)|\
  JS=$(HL_ENABLE_JS)|\
  LUA=$(HL_ENABLE_LUA)|\
  RUNTIME=$(RUNTIME)|\
  EMBED_PLATFORM=$(EMBED_PLATFORM)|\
  APP_BASE_SQLITELESS=$(HL_APP_BASE_SQLITELESS)|\
  APP_BASE_TLSLESS=$(HL_APP_BASE_TLSLESS)|\
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
# release build) that we MUST NOT delete - there's no rule to
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
          $(BUILDDIR)/sandbox.o $(BUILDDIR)/sandbox_tool.o $(BUILDDIR)/signature.o $(BUILDDIR)/release.o $(BUILDDIR)/release_io.o $(BUILDDIR)/tools_install.o $(BUILDDIR)/platform_sig.o \
          $(BUILDDIR)/test_runner.o $(BUILDDIR)/runtime_factory.o $(BUILDDIR)/hull_static.o \
          $(BUILDDIR)/migrate.o $(BUILDDIR)/vfs.o $(BUILDDIR)/cacert.o \
          $(BUILDDIR)/app_context.o $(BUILDDIR)/tool.o $(BUILDDIR)/tool_orchestration.o $(BUILDDIR)/build_assets.o \
          $(BUILDDIR)/compiler.o $(BUILDDIR)/obj_emit.o $(BUILDDIR)/linker_system.o $(BUILDDIR)/linker_lld.o $(BUILDDIR)/linker_zig.o $(BUILDDIR)/bundled_objs.o \
          $(BUILDDIR)/hull_alloc.o $(BUILDDIR)/hull_async.o $(BUILDDIR)/hull_compress.o \
          $(BUILDDIR)/worker_db.o $(BUILDDIR)/worker_wasm.o $(BUILDDIR)/worker_gpu.o \
          $(BUILDDIR)/stdlib_registry.o $(BUILDDIR)/app_entries_default.o \
          $(BUILDDIR)/stdlib_lua_registry.o $(BUILDDIR)/stdlib_js_registry.o \
          $(BUILDDIR)/stdlib_toolchain_registry.o $(BUILDDIR)/runtime_toolchain_registry.o \
          $(BUILDDIR)/stdlib_feature.o $(BUILDDIR)/runtime_cache_common.o \
          $(BUILDDIR)/app_context_runtime.o $(BUILDDIR)/embedded_platform_sig.o \
          $(BUILDDIR)/fs_util.o $(BUILDDIR)/blob_store.o $(BUILDDIR)/cache_dir.o \
          $(BUILDDIR)/cache_registry.o $(BUILDDIR)/host_match.o $(BUILDDIR)/path_normalize.o \
          $(BUILDDIR)/thread_affinity.o $(BUILDDIR)/tls_client.o $(BUILDDIR)/tls_transport.o \
          $(BUILDDIR)/tls_transport_stub.o $(BUILDDIR)/csp.o $(BUILDDIR)/sbom.o \
          $(BUILDDIR)/sh_seal_arena.o \
          $(BUILDDIR)/http_weakstub.o $(BUILDDIR)/wasm_weakstub.o $(BUILDDIR)/image_weakstub.o \
          $(BUILDDIR)/app_runner.o $(BUILDDIR)/build_assets_stub.o \
          $(BUILDDIR)/hull $(PLATFORM_LIB_PURGE) \
          $(BUILDDIR)/test_* 2>/dev/null; \
    printf '%s\n' '$(BUILD_FINGERPRINT)' > $(BUILD_CONFIG_FILE); \
})

# ── Targets ─────────────────────────────────────────────────────────

.PHONY: all clean test debug msan tsan tsan-shared-heap fuzz fuzz-run e2e e2e-build e2e-postgres e2e-mysql e2e-valkey e2e-feature-valkey e2e-http e2e-sandbox e2e-examples e2e-cli e2e-migrate e2e-templates e2e-agent e2e-context e2e-mcp e2e-agent-api e2e-compute e2e-stream-meta e2e-compute-async-trap e2e-sync-spans e2e-compute-aot-shared-heap e2e-compute-memory64 e2e-compute-headers e2e-spans-example e2e-spans-multi e2e-spans-hugefile e2e-compute-dev e2e-aot-cache e2e-cache e2e-cache-concurrent e2e-cache-cosmo e2e-named-connections e2e-dynamic-connections e2e-compiler-free e2e-linker e2e-linker-zig e2e-cross-build e2e-musl e2e-musl-cross floor-musl e2e-build-flavor e2e-install e2e-ca-bundle e2e-update e2e-tools e2e-multipart e2e-attachment e2e-blob e2e-test-harness e2e-jobs e2e-hypermedia-photos-upload e2e-jwt-asym e2e-path-parity hull-test-examples self-build check analyze cppcheck bench bench-template bench-wasm bench-mapped-span bench-gpu bench-bytecode-cache wamrc wamrc-configure coverage lint-lua lint-js lint check-sdk-headers check-sdk-headers-selftest check-wamr-msan-annotation check-docs-integrity check-docs-integrity-selftest check-no-emdash check-no-emdash-selftest check-no-milestone-narration check-no-milestone-narration-selftest check-site-consistency check-site-consistency-selftest platform platform-cosmo hardening check-hardening

all: $(BUILDDIR)/hull

# Hardening summary. Prints which compiler/linker hardening flags the
# Makefile detected as supported by this toolchain. Use `make hardening`
# to see what your build is actually getting.
hardening:
	@echo "Hull hardening summary ($(CC) on $(UNAME_S)/$(UNAME_M)):"
ifdef COSMO
	@echo "  toolchain:        cosmocc (APE) - most ELF hardening flags inapplicable"
	@echo "  stack canary:     skipped (cosmocc default)"
	@echo "  PIE / ASLR:       skipped (APE is its own format)"
	@echo "  RELRO+BIND_NOW:   skipped (no GNU dynamic linker)"
	@echo "  noexecstack:      skipped (APE bootloader handles)"
	@echo "  fortify:          skipped"
	@echo "  CET / BTI:        skipped"
else ifdef HULL_DISABLE_HARDENING
	@echo "  *** HARDENING DISABLED via HULL_DISABLE_HARDENING=1 ***"
	@echo "  Release binaries MUST NOT ship with this flag set."
else
	@echo "  stack canary:     -fstack-protector-strong"
	@echo "  PIE:              -fPIE $(if $(filter -pie,$(LDFLAGS)),(linked with -pie),)"
ifndef DEBUG
	@echo "  fortify:          -D_FORTIFY_SOURCE=3"
else
	@echo "  fortify:          (disabled in DEBUG)"
endif
	@echo "  probed CFLAGS:    $(HARDEN_CFLAGS)"
ifeq ($(UNAME_S),Linux)
	@echo "  Linux LDFLAGS:    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack $(HARDEN_LDFLAGS)"
endif
	@echo "  link-time:        $(call hl_have_ldflag,-Wl$(comma)--as-needed)"
ifeq ($(HL_ENABLE_LTO),1)
ifneq ($(HL_LTO_CFLAG),)
	@echo "  LTO:              $(HL_LTO_CFLAG) (HL_ENABLE_LTO=1)"
else
	@echo "  LTO:              probe failed (HL_ENABLE_LTO=1 requested but $(CC) rejected -flto*)"
endif
else
	@echo "  LTO:              disabled (set HL_ENABLE_LTO=1 to enable)"
endif
ifeq ($(HL_ENABLE_CFI),1)
ifneq ($(HL_CFI_CFLAG),)
	@echo "  CFI:              $(HL_CFI_CFLAG) $(HL_CFI_MODE) -fsplit-lto-unit (HL_ENABLE_CFI=1)"
else
	@echo "  CFI:              probe failed (HL_ENABLE_CFI=1 requested but $(CC) does not support -fsanitize=cfi-icall on this target - Linux clang ≥ 7.0 only)"
endif
else
	@echo "  CFI:              disabled (set HL_ENABLE_CFI=1 to enable; Linux clang only)"
endif
endif

# Post-build hardening verifier. Runs scripts/check_hardening.sh
# against build/hull. Exits non-zero in release/Linux builds if
# required protections are missing; prints "skipped" for properties
# this platform can't enforce.
check-hardening: $(BUILDDIR)/hull
	@scripts/check_hardening.sh $(BUILDDIR)/hull

# Platform static library - everything except entry.o and build_assets.o
# Used by `hull build` to produce standalone app binaries.
# Exports hull_main() (subcommand dispatch + server logic).
# EMBEDDED_PLATFORM_SIG_OBJ is included in PLATFORM_OBJS because
# mod_tool.c (in $(RT_OBJS)) references the hl_embedded_platform_sig
# accessor. Apps linking libhull_platform.a need the symbol resolved
# at link time even though tool.platform_sig_get is only ever called
# in tool mode (hull build, hull verify), never from app runtime.
# Cost: ~hundreds of bytes per app (the embedded manifest+sig, dead
# weight at app runtime). Trade we accept for a clean symbol graph.
# The NATIVE base platform lib is RUNTIME-LESS: no interpreter, no
# per-runtime stdlib/manifest. A produced app composes exactly one runtime
# archive (libhull_feature-<rt>.a). COSMO stays DUAL: features are
# native-only static archives, so a fat-APE app cannot compose one - the
# cosmo base embeds both runtimes (full, no slim). The hull toolchain link
# (below) stays dual on every target.
# HTTP feature (core caps + web bindings + embed) moved to mk/features/http.mk
include mk/features/http.mk

# WASM feature (archive + compute bridges + embed) moved to mk/features/wasm.mk
include mk/features/wasm.mk
# Image feature (FEATURE_IMAGE_OBJS + core/bridge archives + IMG_FEATURE_* +
# embedded_image.h). Included here, before PLATFORM_CAP_OBJS / RUNTIME_FEATURE_
# LIBS / BUILD_ASSET_OBJ below reference its vars; the fragment self-gates on
# HL_ENABLE_IMAGE.
include mk/features/image.mk
# TLS as a composable feature (docs/tls_feature.md, a2). The mbedTLS crypto
# backends (strong overrides of the weak hl_crypto_*_active_backend hooks) leave
# the base under HL_TLS_FEATURE=1, composed back from libhull_feature-tls.a; the
# base keeps crypto.o's portable/stub weak defaults. Empty on a normal build so
# the base stays TLS-full. (tls_client.o / tls_transport.o are dropped via their
# own OBJ vars above; mbedTLS via MBEDTLS_OBJS.)
ifeq ($(HL_TLS_FEATURE),1)
FEATURE_TLS_CAP_OBJS := $(BUILDDIR)/cap_crypto_hmac_mbedtls.o $(BUILDDIR)/cap_crypto_asym_mbedtls.o
else
FEATURE_TLS_CAP_OBJS :=
endif
ifdef COSMO
  PLATFORM_RT_OBJS       := $(RT_OBJS)
  PLATFORM_MANIFEST_OBJ  := $(MANIFEST_OBJ)
  PLATFORM_RUNTIME_EXTRA := $(STDLIB_RT_REGISTRY_OBJS) $(STDLIB_TOOLCHAIN_REGISTRY_O) $(RUNTIME_TOOLCHAIN_REGISTRY_O) $(VEND_OBJS)
  PLATFORM_CAP_OBJS      := $(CAP_OBJS)                 # cosmo stays full (features native-only)
  PLATFORM_STB_OBJ       := $(STB_OBJ)                  # cosmo base bundles stb (cap_image stays in-base)
else
  PLATFORM_RT_OBJS       := $(RUNTIME_CACHE_COMMON_OBJ)  # runtime-agnostic; VMs dropped
  PLATFORM_MANIFEST_OBJ  := $(BUILDDIR)/manifest.o       # runtime-agnostic
  PLATFORM_RUNTIME_EXTRA :=
  # HTTP core caps move to libhull_feature-http.a; the wasm caps move to
  # libhull_feature-wasm.a (docs/wasm_feature.md); the image codec caps
  # move to libhull_feature-image.a (docs/image_feature.md). All compose back at
  # `hull build`; the base keeps the weak stubs (http/wasm/image_weakstub).
  PLATFORM_CAP_OBJS      := $(filter-out $(FEATURE_HTTP_OBJS) $(FEATURE_WASM_OBJS) $(FEATURE_IMAGE_OBJS) $(FEATURE_TLS_CAP_OBJS),$(CAP_OBJS))
  PLATFORM_STB_OBJ       :=                              # stb moves into libhull_feature-image.a
endif
PLATFORM_OBJS := $(PLATFORM_CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(PLATFORM_RT_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_GPU_OBJ) $(PLATFORM_MANIFEST_OBJ) $(MODULE_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(SANDBOX_OBJ) $(SANDBOX_TOOL_OBJ) $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(CSP_OBJ) $(SBOM_OBJ) $(STDLIB_FEATURE_OBJ) $(APP_CONTEXT_OBJ) $(APP_CONTEXT_RT_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(MAIN_OBJ) $(SERVE_OBJ) $(APP_RUNNER_OBJ) $(HTTP_WEAKSTUB_OBJ) $(WASM_WEAKSTUB_OBJ) $(IMAGE_WEAKSTUB_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_STUB_OBJ) $(STDLIB_REGISTRY_O) $(PLATFORM_RUNTIME_EXTRA) $(MBEDTLS_OBJS) \
	$(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(PLATFORM_STB_OBJ) $(PLEDGE_OBJS) \
	$(COMPILER_OBJ) $(OBJ_EMIT_OBJ) $(LINKER_SYSTEM_OBJ) $(LINKER_LLD_OBJ) $(LINKER_ZIG_OBJ) $(BUNDLED_OBJS_OBJ)

PLATFORM_LIB := $(BUILDDIR)/libhull_platform.a

# Platform canary - embeds an integrity hash so the browser verifier can
# detect whether the Hull platform is actually present in the binary.
CANARY_C    := $(BUILDDIR)/platform_canary.c
CANARY_OBJ  := $(BUILDDIR)/platform_canary.o
CANARY_HASH := $(BUILDDIR)/platform_canary_hash

$(CANARY_C): $(PLATFORM_OBJS) | $(BUILDDIR)
	@hash=$$(cat $(sort $(PLATFORM_OBJS)) | $(SHA256CMD) | cut -d' ' -f1) && \
	echo "$$hash" > $(CANARY_HASH) && \
	bytes=$$(echo "$$hash" | fold -w2 | awk '{printf "%s0x%s",(NR>1?",":""),$$0}') && \
	printf '/* Auto-generated platform canary - do not edit */\n#include <stdint.h>\nconst struct { char magic[24]; uint8_t integrity[32]; } hl_platform_canary = {\n    "HULL_PLATFORM_CANARY",\n    {%s}\n};\n' "$$bytes" > $@

$(CANARY_OBJ): $(CANARY_C) | $(BUILDDIR)
	$(CC) -std=c11 -O2 -w -c -o $@ $<

# When TRUST_PLATFORM_LIB=1, treat $(PLATFORM_LIB) as a pre-built
# leaf - make doesn't re-link it from source prereqs. This is the
# release-time path: CI downloads the .a artifact that
# sign-platform-manifest hashed and we MUST embed those exact bytes
# (touch+mtime tricks aren't reliable enough - they didn't survive
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
# The platform objects reference sh_seal_arena (Hull's manifest seal / sealed
# runtime tables; vfs.o -- which IS in this archive -- calls it). Pre-Keel-v3,
# that symbol arrived bundled inside the merged keel archive, so it only needed
# adding here in the no-keel (pure-compute) case. Keel v3 no longer ships
# sh_seal_arena (Hull vendors its own under vendor/sh_seal_arena/), so add
# Hull's own instrumented sh_seal_arena.o UNCONDITIONALLY -- a composed app
# (hull build) links only this archive and must resolve the symbol from it.
PLATFORM_SEAL_ARENA_OBJ := $(SH_SEAL_ARENA_OBJ)
$(PLATFORM_LIB): $(PLATFORM_OBJS) $(CANARY_OBJ) $(PLATFORM_SEAL_ARENA_OBJ) $(KEEL_LIB) | $(BUILDDIR)
	@rm -f $@
	$(AR) rcs $@ $(PLATFORM_OBJS) $(CANARY_OBJ) $(PLATFORM_SEAL_ARENA_OBJ)
	@# Merge keel objects into the platform archive. KEEL_LIB is empty when
	@# both HTTP halves are off (pure-compute flavor), in which case there is
	@# nothing to merge -- skip rather than `ar x` the empty path (a dir).
	@if [ -n "$(KEEL_LIB)" ]; then \
		tmpdir=$$(mktemp -d) && \
		cd $$tmpdir && \
		$(AR) x $(CURDIR)/$(KEEL_LIB) && \
		$(if $(filter 1,$(HL_TLS_FEATURE)),rm -f tls_mbedtls.o &&,) \
		$(AR) rcs $(CURDIR)/$@ *.o && \
		rm -rf $$tmpdir ; \
	fi
	@# Record the CC used so hull build can auto-detect
	@echo "$(CC)" > $(BUILDDIR)/platform_cc
endif

platform: $(PLATFORM_LIB)

# ── Build flavors are now build.lua PRESETS, not pre-built platform libs ──
# `pure-compute` (the only non-full flavor) is a preset
# (docs/keel_feature.md): it builds on the DEFAULT composable base -- which drops
# HTTP/TLS/Keel and composes each back per app -- and only validates that the app
# declares no HTTP/TLS. A compute app on that base already links zero
# HTTP/Keel/mbedTLS, so the old pre-built `platform-pure-compute` archive +
# `hull flavor install pure-compute` are gone. The KEELLESS/TLSLESS/SLIM
# app-build bases below are the composable-base sub-builds, not user-facing
# flavors.

# ── SQLite-less app-build base (HL_APP_BASE_SQLITELESS=1) ──────
# The platform lib the distributed hull embeds as the DEFAULT app-build base.
# Built in a dedicated object dir at HL_SQLITE_FEATURE=1 (SQLite dropped, DB core
# intact) so it never clobbers the main sqlite-full build or build/hull. The
# EMBED_PLATFORM path xxd's this instead of the sqlite-full libhull_platform.a
# when HL_APP_BASE_SQLITELESS=1. See docs/sqlite_feature.md.
SQLITELESS_PLATFORM_LIB := $(BUILDDIR)/libhull_platform-sqliteless.a
ifeq ($(TRUST_PLATFORM_LIB),1)
# Release stage 3: the SQLite-less base was downloaded from build-platform-native
# (the exact bytes sign-platform-manifest hashed). Trust it as-is, like
# $(PLATFORM_LIB) above; never sub-build over the signed artifact.
$(SQLITELESS_PLATFORM_LIB): | $(BUILDDIR)
	@test -f $@ || (echo "ERROR: TRUST_PLATFORM_LIB=1 but $@ is missing"; exit 1)
	@echo "$@: trusting pre-built artifact (TRUST_PLATFORM_LIB=1)"
else
$(SQLITELESS_PLATFORM_LIB):
	$(MAKE) platform BUILDDIR=$(BUILDDIR)/sqliteless HL_SQLITE_FEATURE=1
	cp $(BUILDDIR)/sqliteless/libhull_platform.a $@
	@echo "built $@ (SQLite-less app-build base)"
endif
.PHONY: platform-sqliteless
platform-sqliteless: $(SQLITELESS_PLATFORM_LIB)

# ── TLS-less app-build base (a2, HL_APP_BASE_TLSLESS=1) ─────────────────
# The platform lib the distributed hull will embed as the app-build base once
# the compose is wired (docs/tls_feature.md, a2-main part 2). Built in a
# dedicated object dir at HL_TLS_FEATURE=1 (mbedTLS + the mbedTLS-consuming TUs
# dropped, Keel + HTTP core intact) so it never clobbers the main TLS-full build
# or build/hull. `make platform-tlsless` builds + verifies it is mbedTLS-free.
TLSLESS_PLATFORM_LIB := $(BUILDDIR)/libhull_platform-tlsless.a
ifeq ($(TRUST_PLATFORM_LIB),1)
$(TLSLESS_PLATFORM_LIB): | $(BUILDDIR)
	@test -f $@ || (echo "ERROR: TRUST_PLATFORM_LIB=1 but $@ is missing"; exit 1)
	@echo "$@: trusting pre-built artifact (TRUST_PLATFORM_LIB=1)"
else
$(TLSLESS_PLATFORM_LIB):
	$(MAKE) platform BUILDDIR=$(BUILDDIR)/tlsless HL_TLS_FEATURE=1
	cp $(BUILDDIR)/tlsless/libhull_platform.a $@
	@echo "built $@ (TLS-less app-build base)"
endif
.PHONY: platform-tlsless
platform-tlsless: $(TLSLESS_PLATFORM_LIB)
	@echo "verifying $(TLSLESS_PLATFORM_LIB) is mbedTLS-free..."
	@if [ "$$(ar t $(TLSLESS_PLATFORM_LIB) | grep -cE '^mbed_')" != "0" ]; then \
		echo "FAIL: TLS-less base still bundles mbedTLS objects"; exit 1; fi
	@if nm $(TLSLESS_PLATFORM_LIB) 2>/dev/null | grep -qE ' [TtWw] _?mbedtls_ssl_handshake'; then \
		echo "FAIL: TLS-less base defines mbedtls_ssl_handshake"; exit 1; fi
	@echo "ok  TLS-less base carries no mbedTLS (a2 base-drop verified)"

# ── Keel-less app-build base (HL_KEEL_FEATURE=1) ────────────
# The event-loop half of the composable base: serve.o (KlServer loop) +
# async/keel.c (Keel event loop) + net/keel.c (Keel net backend) leave the base
# object set, replaced by the Keel-free serve_cli.o entry + the weak poll backend
# + weak net stubs. serve.o/keel.c/net_keel.o compose back (strong overrides) in
# the whole-archived http feature on needs_http; libkeel stays merged in the base
# .a (pulled on-demand by a composed serve.o). A compute app references no kl_*
# and links Keel-free. Combined with HL_TLS_FEATURE it is a genuinely
# Keel-and-mbedTLS-free base. `make platform-keelless` builds + verifies the
# event-loop objects are gone. Compose wiring (http feature + app-base) is the
# next step; the hull binary + plain `make` are unaffected. See docs/keel_feature.md.
KEELLESS_PLATFORM_LIB := $(BUILDDIR)/libhull_platform-keelless.a
ifeq ($(TRUST_PLATFORM_LIB),1)
$(KEELLESS_PLATFORM_LIB): | $(BUILDDIR)
	@test -f $@ || (echo "ERROR: TRUST_PLATFORM_LIB=1 but $@ is missing"; exit 1)
	@echo "$@: trusting pre-built artifact (TRUST_PLATFORM_LIB=1)"
else
$(KEELLESS_PLATFORM_LIB):
	$(MAKE) platform BUILDDIR=$(BUILDDIR)/keelless HL_KEEL_FEATURE=1
	cp $(BUILDDIR)/keelless/libhull_platform.a $@
	@echo "built $@ (Keel-less app-build base)"
endif
.PHONY: platform-keelless
platform-keelless: $(KEELLESS_PLATFORM_LIB)
	@echo "verifying $(KEELLESS_PLATFORM_LIB) drops the Keel event loop..."
	@for o in serve.o async_keel.o net_keel.o; do \
		if [ "$$(ar t $(KEELLESS_PLATFORM_LIB) | grep -cE "^$$o\$$")" != "0" ]; then \
			echo "FAIL: Keel-less base still bundles $$o"; exit 1; fi; done
	@if [ "$$(ar t $(KEELLESS_PLATFORM_LIB) | grep -cE '^serve_cli\.o$$')" != "1" ]; then \
		echo "FAIL: Keel-less base is missing the serve_cli.o entry"; exit 1; fi
	@echo "ok  Keel-less base drops serve.o + async_keel.o + net_keel.o (4.2b base-drop verified)"

# ── Combined SLIM app-build base: SQLite-less + TLS-less + Keel-less ────
# The release's minimal app-build base: every composable subsystem dropped, each
# composed back per app. One shared sub-build (the embedded base is a single
# platform lib): HL_SQLITE_FEATURE=1 + HL_TLS_FEATURE=1 + HL_KEEL_FEATURE=1. Used
# when HL_APP_BASE_SQLITELESS=1 AND HL_APP_BASE_TLSLESS=1 (the release sets both).
SLIM_PLATFORM_LIB := $(BUILDDIR)/libhull_platform-slim.a
ifeq ($(TRUST_PLATFORM_LIB),1)
$(SLIM_PLATFORM_LIB): | $(BUILDDIR)
	@test -f $@ || (echo "ERROR: TRUST_PLATFORM_LIB=1 but $@ is missing"; exit 1)
	@echo "$@: trusting pre-built artifact (TRUST_PLATFORM_LIB=1)"
else
$(SLIM_PLATFORM_LIB):
	$(MAKE) platform BUILDDIR=$(BUILDDIR)/slim HL_SQLITE_FEATURE=1 HL_TLS_FEATURE=1 HL_KEEL_FEATURE=1
	cp $(BUILDDIR)/slim/libhull_platform.a $@
	@echo "built $@ (SQLite-less + TLS-less + Keel-less app-build base)"
endif
.PHONY: platform-slim
platform-slim: $(SLIM_PLATFORM_LIB)

# DuckDB --with feature moved to mk/features/duckdb.mk
include mk/features/duckdb.mk

# PostgreSQL --with feature moved to mk/features/postgres.mk
include mk/features/postgres.mk

# MySQL --with feature moved to mk/features/mysql.mk
include mk/features/mysql.mk

# Valkey/Redis KV --with feature (the first non-SQL connection feature)
include mk/features/valkey.mk

# SQLite feature (archive + udf bridges + both embeds) moved to mk/features/sqlite.mk
include mk/features/sqlite.mk

# GPU --with feature moved to mk/features/gpu.mk
include mk/features/gpu.mk


# TRUST_FEATURE_LIBS=1 (release stage 3, issue #114): the embedded runtime /
# HTTP-core / web-bindings / tui-bridge feature archives were downloaded from
# build-platform-native - the EXACT bytes sign-platform-manifest hashed into the
# signed platform manifest. Embed them as-is so the runtime composed-feature
# check (signature.c §5c) matches the embedded manifest. AR_FEATURE_LIB's trust
# branch only asserts presence; it never re-archives, so the signed bytes survive
# into hull even if source objects recompile (harmless - they're never used).
# Local devs / normal `make` leave it unset and build the archives from source.
# NOTE: applies ONLY to the seven archives EMBEDDED in a native hull. The tui CORE
# (libhull_feature-tui.a) and the --with backend features are release-domain
# (hull.sha256), not covered here.
ifeq ($(TRUST_FEATURE_LIBS),1)
define AR_FEATURE_LIB
	@test -f $@ || (echo "ERROR: TRUST_FEATURE_LIBS=1 but $@ is missing"; exit 1)
	@echo "$@: trusting pre-built artifact (TRUST_FEATURE_LIBS=1)"
endef
else
define AR_FEATURE_LIB
	@rm -f $@
	$(AR) rcs $@ $(1)
	@echo "built $@ ($$(du -h $@ | cut -f1))"
endef
endif

# TUI feature (cap core + runtime bridges + embed) moved to mk/features/tui.mk
include mk/features/tui.mk



# Keel event-loop feature (archive + SLIM-base embed) moved to mk/features/keel.mk
include mk/features/keel.mk


# Per-runtime SQLite UDF bridges (mod_db_udf). Tiny (one object each); the sole
# per-runtime sqlite3_* consumer, split out of mod_db so the runtime archive is
# SQLite-free (docs/sqlite_feature.md). Embedded in hull + composed
# for the app's runtime whenever the app uses a udf-capable DB. Force
# -DHL_ENABLE_SQLITE so the bridge carries the bindings even on a SQLite-less
# feature base (HL_SQLITE_FEATURE=1), resolving sqlite3_* from the composed
# engine -- but ONLY when SQLite is reachable. A genuine no-SQLite build
# (postgres/mysql-only: HL_ENABLE_SQLITE=0 and no feature) compiles the bridge
# EMPTY (guarded out), so it carries no unresolvable sqlite3_* refs into the hull
# binary (mod_db_udf.o is in RT_OBJS) or the archive.
ifneq ($(filter 1,$(HL_ENABLE_SQLITE) $(HL_SQLITE_FEATURE)),)
$(BUILDDIR)/lua_rt_mod_db_udf.o $(BUILDDIR)/js_mod_db_udf.o: CFLAGS += -DHL_ENABLE_SQLITE
endif


# (Image feature moved to mk/features/image.mk - included above near the
# FEATURE_*_OBJS cluster, before the PLATFORM_CAP_OBJS aggregate references it.)

# TLS feature (archive + embed) moved to mk/features/tls.mk
include mk/features/tls.mk

# (IMG_FEATURE_* registration vars moved to mk/features/image.mk.)

# Lua + JS runtime features (both archives + shared embed) moved to mk/features/runtime.mk
include mk/features/runtime.mk


# Rebuild every feature archive when the Makefile changes. The archives are
# `ar rcs` over an object LIST (FEATURE_*_OBJS / the tui trio), and those lists
# live here in the Makefile. `ar rcs` (with the recipe's `rm`) refreshes an
# archive only when the recipe RUNS, which make triggers only when a member .o
# is newer than the archive - NOT when the list itself changes (a member moved
# between archives, or a source added to a filter, without touching any .o). A
# stale member would then linger and surface as a `duplicate symbol` at compose.
# The lists change only via a Makefile edit, so depending on the Makefile forces
# the (cheap) rebuild exactly then. CI clean-builds are unaffected.
# Derived from the registry (mk/feature.mk FEATURE_EMBEDDED_STEMS) rather than
# hand-listed: the hand-list had drifted and OMITTED the tls, keel, and sqlite
# (core) embedded archives, so those three missed this rebuild guard (the exact
# stale-`ar` duplicate-symbol hazard the comment above warns about). Plus the
# tui CORE archive (installable, but built here from an object list in this
# Makefile, so it needs the guard too). FEATURE_EMBEDDED_LIBS already covers
# lua/js/http(+rt)/wasm(+rt)/sqlite(+rt)/image(+rt)/tls/keel + the tui rt bridges.
FEATURE_ARCHIVES := $(FEATURE_EMBEDDED_LIBS) $(BUILDDIR)/libhull_feature-tui.a
$(FEATURE_ARCHIVES): Makefile
# The base platform lib is built from an object LIST (PLATFORM_OBJS); a Phase-1
# change to that list (wasm caps + WAMR removed) must retrigger the ar, which an
# mtime check alone misses (docs/wasm_feature.md). Same guard as the features.
$(PLATFORM_LIB): Makefile

# Runtime feature archives a native `hull build` needs to compose a runnable
# app. The native base is runtime-less, so `hull build` resolves the runtime
# from build/libhull_feature-<rt>.a (or an embedded copy, or ~/.hull/feature).
# Building them alongside hull makes `make && hull build` work with no extra
# step (and gives every e2e that shells out to `hull build` its runtime). Cosmo
# has a dual base and needs none; a single-runtime build gets only its half.
#
# The native base is also HTTP-core-less (issue #114), so `hull build` composes
# libhull_feature-http.a for every full-flavor app; build it here too so a plain
# `make && hull build` resolves it from build/ with no extra step. Cosmo keeps
# HTTP in the base and composes no http feature.
ifndef COSMO
ifeq ($(RUNTIME),js)
  RUNTIME_FEATURE_LIBS := $(BUILDDIR)/libhull_feature-js.a $(BUILDDIR)/libhull_feature-http-js.a \
                          $(BUILDDIR)/libhull_feature-wasm-js.a $(BUILDDIR)/libhull_feature-sqlite-js.a \
                          $(IMG_FEATURE_JS)
else ifeq ($(RUNTIME),lua)
  RUNTIME_FEATURE_LIBS := $(BUILDDIR)/libhull_feature-lua.a $(BUILDDIR)/libhull_feature-http-lua.a \
                          $(BUILDDIR)/libhull_feature-wasm-lua.a $(BUILDDIR)/libhull_feature-sqlite-lua.a \
                          $(IMG_FEATURE_LUA)
else
  RUNTIME_FEATURE_LIBS := $(BUILDDIR)/libhull_feature-lua.a $(BUILDDIR)/libhull_feature-http-lua.a \
                          $(BUILDDIR)/libhull_feature-wasm-lua.a $(BUILDDIR)/libhull_feature-sqlite-lua.a \
                          $(IMG_FEATURE_LUA) \
                          $(BUILDDIR)/libhull_feature-js.a $(BUILDDIR)/libhull_feature-http-js.a \
                          $(BUILDDIR)/libhull_feature-wasm-js.a $(BUILDDIR)/libhull_feature-sqlite-js.a \
                          $(IMG_FEATURE_JS)
endif
  # HTTP + WASM + IMAGE core feature archives (runtime-agnostic), composed for
  # every full-flavor app (docs/wasm_feature.md; wasm always composes).
  # IMG_FEATURE_CORE is empty on an image-less base (HL_ENABLE_IMAGE=0).
  RUNTIME_FEATURE_LIBS += $(BUILDDIR)/libhull_feature-http.a $(BUILDDIR)/libhull_feature-wasm.a \
                          $(IMG_FEATURE_CORE)
else
  RUNTIME_FEATURE_LIBS :=
endif

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

# (Per-flavor cosmo platform libs removed: pure-compute is a
# build.lua preset on the default composable base, not a pre-built flavor lib.)

# ── wamrc AOT compiler ──────────────────────────────────────────────
#
# Build the WAMR AOT compiler from vendor/wamr/wamr-compiler.
# Requires: cmake, LLVM (brew install llvm on macOS, apt install llvm on Linux).
# Output: build/wamrc
# Override LLVM path: make wamrc WAMRC_CMAKE_FLAGS="-DLLVM_DIR=/path/to/llvm/cmake"

WAMRC_BUILD_DIR := $(BUILDDIR)/wamrc-build

# Configure-only: generate the CMake build tree (incl. CMakeCache.txt, which
# records the resolved C/C++ compiler paths + LLVM) WITHOUT compiling wamrc. The
# Slice-5A artifact CONSUMER runs this to reproduce the producer's toolchain
# identity for cold artifact verification - i.e. verifying BEFORE (and without)
# building wamrc from source (docs/ci_architecture_design.md D.1.3).
wamrc-configure: $(WAMR_PATCH_PREREQ) | $(BUILDDIR)
	@echo "=== Configuring wamrc build tree (no compile) ==="
	@mkdir -p $(WAMRC_BUILD_DIR)
	@cd $(WAMRC_BUILD_DIR) && cmake $(CURDIR)/$(WAMR_DIR)/wamr-compiler \
		-DCMAKE_BUILD_TYPE=Release \
		-DWAMR_BUILD_WITH_CUSTOM_LLVM=1 \
		-DWASM_ENABLE_INSTRUCTION_METERING=1 \
		$(WAMRC_CMAKE_FLAGS) 2>&1 | tail -5

wamrc: wamrc-configure | $(BUILDDIR)
	@echo "=== Building wamrc AOT compiler ==="
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
# Multi-arch cosmo embedding - xxd both archives + metadata table
$(EMBEDDED_PLATFORM_H): $(BUILDDIR)/libhull_platform.x86_64-cosmo.a \
                         $(BUILDDIR)/libhull_platform.aarch64-cosmo.a | $(BUILDDIR)
	@echo "/* Auto-generated multi-arch - do not edit */" > $@
	xxd -i $(BUILDDIR)/libhull_platform.x86_64-cosmo.a | \
		sed 's/build_libhull_platform_x86_64_cosmo_a/hl_platform_x86_64_cosmo/g' | \
		$(XXD_CONST_PIPE) >> $@
	xxd -i $(BUILDDIR)/libhull_platform.aarch64-cosmo.a | \
		sed 's/build_libhull_platform_aarch64_cosmo_a/hl_platform_aarch64_cosmo/g' | \
		$(XXD_CONST_PIPE) >> $@
	@echo "" >> $@
	@echo "static const HlEmbeddedPlatform hl_embedded_platforms[] = {" >> $@
	@echo '    { "x86_64-cosmo", hl_platform_x86_64_cosmo, sizeof(hl_platform_x86_64_cosmo) },' >> $@
	@echo '    { "aarch64-cosmo", hl_platform_aarch64_cosmo, sizeof(hl_platform_aarch64_cosmo) },' >> $@
	@echo "    { NULL, NULL, 0 }" >> $@
	@echo "};" >> $@

$(EMBEDDED_TEMPLATES_H): templates/app_main.c templates/entry.h | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i templates/app_main.c | sed 's/templates_app_main_c/hl_embedded_app_main_c/g' | $(XXD_CONST_PIPE) >> $@
	@xxd -i templates/entry.h | sed 's/templates_entry_h/hl_embedded_entry_h/g' | $(XXD_CONST_PIPE) >> $@

CFLAGS += -DHL_BUILD_EMBEDDED -DHL_BUILD_EMBEDDED_MULTIARCH
$(BUILD_ASSET_OBJ): $(EMBEDDED_PLATFORM_H) $(EMBEDDED_TEMPLATES_H)

else ifneq ($(EMBED_PLATFORM),)
# Single-arch embedding (existing behavior). The app-build base is the
# sqlite-full platform lib by default; HL_APP_BASE_SQLITELESS=1 swaps
# in the SQLite-less sub-build so produced apps drop SQLite. Either way the hull
# binary itself stays sqlite-full (it links its own objects, not this archive).
# The embedded app-build base: full, or a sub-build with composable subsystems
# dropped per HL_APP_BASE_{SQLITELESS,TLSLESS} (each composed back at hull build).
# The four combinations map to four sub-build libs so a release can drop both.
ifeq ($(HL_APP_BASE_SQLITELESS)$(HL_APP_BASE_TLSLESS),00)
APP_BASE_LIB := $(PLATFORM_LIB)                # full
else ifeq ($(HL_APP_BASE_SQLITELESS)$(HL_APP_BASE_TLSLESS),10)
APP_BASE_LIB := $(SQLITELESS_PLATFORM_LIB)     # SQLite dropped
else ifeq ($(HL_APP_BASE_SQLITELESS)$(HL_APP_BASE_TLSLESS),01)
APP_BASE_LIB := $(TLSLESS_PLATFORM_LIB)        # TLS dropped
else
APP_BASE_LIB := $(SLIM_PLATFORM_LIB)           # both dropped (combined sub-build)
endif
# The xxd symbol carries the archive's basename suffix (_sqliteless/_tlsless/_slim/
# none); rename any of them to the canonical hl_embedded_platform_a.
$(EMBEDDED_PLATFORM_H): $(APP_BASE_LIB) | $(BUILDDIR)
	xxd -i $< | sed 's/build_libhull_platform[a-z_]*_a/hl_embedded_platform_a/g' | $(XXD_CONST_PIPE) > $@

$(EMBEDDED_TEMPLATES_H): templates/app_main.c templates/entry.h | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i templates/app_main.c | sed 's/templates_app_main_c/hl_embedded_app_main_c/g' | $(XXD_CONST_PIPE) >> $@
	@xxd -i templates/entry.h | sed 's/templates_entry_h/hl_embedded_entry_h/g' | $(XXD_CONST_PIPE) >> $@



# Embed the per-runtime tui bridges too (issue #114). The tui cap core
# stays the single installable feature asset; a full-flavor `--with=tui` app
# composes the cap core (installed/local) + its runtime's bridge (embedded here),
# so `hull feature install tui` still fetches one archive.




CFLAGS += -DHL_BUILD_EMBEDDED -DHL_BUILD_EMBEDDED_RUNTIME -DHL_BUILD_EMBEDDED_HTTP -DHL_BUILD_EMBEDDED_TUI -DHL_BUILD_EMBEDDED_WASM -DHL_BUILD_EMBEDDED_SQLITE_RT -DHL_BUILD_EMBEDDED_IMAGE
$(BUILD_ASSET_OBJ): $(EMBEDDED_PLATFORM_H) $(EMBEDDED_TEMPLATES_H) $(EMBEDDED_RUNTIME_H) $(EMBEDDED_HTTP_H) $(EMBEDDED_TUI_H) $(EMBEDDED_WASM_H) $(EMBEDDED_SQLITE_RT_H) $(EMBEDDED_IMAGE_H)



endif

# Hull binary
#
# The object list is defined once as HULL_LINK_OBJS and referenced by BOTH the
# prerequisite line and the link recipe (it was previously written out verbatim
# in both - the same ~70-token list twice for one target, an easy drift point).
# Order is preserved exactly, so the produced binary is byte-identical. The
# prereq / recipe TAILS legitimately differ (order-only feature-lib prereqs vs
# the -l link flags), so they stay inline. (The larger cross-target sharing with
# PLATFORM_OBJS is deliberately NOT collapsed: the shared vars are interleaved
# with each list's distinct ones, so a shared-core extraction would REORDER the
# link line and risk weak/strong seam resolution - see docs/build_arc_audit.md.)
HULL_LINK_OBJS := $(CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(RT_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(MANIFEST_OBJ) $(MODULE_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(SANDBOX_OBJ) $(SANDBOX_TOOL_OBJ) $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(CSP_OBJ) $(SH_SEAL_ARENA_OBJ) $(SBOM_OBJ) $(STDLIB_FEATURE_OBJ) $(APP_CONTEXT_OBJ) $(APP_CONTEXT_RT_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(TOOL_OBJ) $(BUILD_ASSET_OBJ) $(COMPILER_OBJ) $(OBJ_EMIT_OBJ) $(LINKER_SYSTEM_OBJ) $(LINKER_LLD_OBJ) $(LINKER_ZIG_OBJ) $(BUNDLED_OBJS_OBJ) $(MAIN_OBJ) $(SERVE_OBJ) $(ENTRY_OBJ) $(APP_EXTRA_OBJS) $(STDLIB_REGISTRY_O) $(STDLIB_RT_REGISTRY_OBJS) $(STDLIB_TOOLCHAIN_REGISTRY_O) $(RUNTIME_TOOLCHAIN_REGISTRY_O) $(WAMR_OBJS) $(VEND_OBJS) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS)

# The JS source-frontend tooling runtime needs QuickJS -> linked into the hull binary only
# when this build links QuickJS (RUNTIME=js or the default both; NOT a lua-only hull). The
# generation manager rides with it, and HL_FRONTEND_JS enables the tool-VM bridge bindings
# (mod_tool.c stays compilable in a lua-only build; the bindings then report unavailable).
ifneq ($(RUNTIME),lua)
  HULL_LINK_OBJS += $(FRONTEND_JS_SESSION_OBJ) $(FRONTEND_JS_GEN_OBJ) $(STDLIB_JS_CLI_REGISTRY_O)
  CFLAGS += -DHL_FRONTEND_JS
  # Same objects a test binary needs when it links the tool bindings (lua_rt_mod_tool.o's
  # frontend bridge) or the tool-VM teardown (tool.o's hl_js_gen_shutdown). Empty on a
  # lua-only build, where those references are #ifdef'd out. Consumed by mk/tests.mk.
  FRONTEND_JS_LINK_OBJS := $(FRONTEND_JS_SESSION_OBJ) $(FRONTEND_JS_GEN_OBJ) $(STDLIB_JS_CLI_REGISTRY_O)
endif

$(BUILDDIR)/hull: $(HULL_LINK_OBJS) $(KEEL_LIB) $(TUI_TOOLCHAIN_ARCHIVE) | $(RUNTIME_FEATURE_LIBS)
	$(CC) $(LDFLAGS) -o $@ $(HULL_LINK_OBJS) $(KEEL_LIB) $(TUI_TOOLCHAIN_LDFLAGS) $(WGPU_LIB) $(WGPU_FRAMEWORKS) $(DUCKDB_LIBS) -lm -lpthread

# libhull no-runtime embedding library lives in mk/libhull.mk.
include mk/libhull.mk

# Capability sources
$(BUILDDIR)/cap_%.o: $(SRCDIR)/hull/cap/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Command module sources
$(BUILDDIR)/cmd_%.o: $(SRCDIR)/hull/commands/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Async backend implementations. Future net/ + http_client/
# subdirs will get their own pattern rules alongside this one.
$(BUILDDIR)/async_%.o: $(SRCDIR)/hull/async/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Net backend implementations
$(BUILDDIR)/net_%.o: $(SRCDIR)/hull/net/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# JS runtime sources
$(BUILDDIR)/js_%.o: $(SRCDIR)/hull/runtime/js/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Lua runtime sources
$(BUILDDIR)/lua_rt_%.o: $(SRCDIR)/hull/runtime/lua/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Hull allocator
$(ALLOC_OBJ): $(SRCDIR)/hull/utils/alloc.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Hull async glue (runtime-agnostic HlAsyncCtx + callbacks)
$(ASYNC_OBJ): $(SRCDIR)/hull/shared/async.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Hull compression helper
$(COMPRESS_OBJ): $(SRCDIR)/hull/utils/compress.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# miniz (vendored compression library: relaxed warnings via -w, but
# the full probe-based hardening set (stack-protector, FORTIFY,
# fno-plt, trivial-auto-var-init, zero-call-used-regs, BTI/CET) still
# applies so ROP gadgets in miniz's text segment are protected the
# same way first-party Hull TUs are.  Without this, a heap-write
# primitive that lands in miniz's decompression state inherits a
# wide gadget surface.  Baseline (stack-protector + PIE) is gated
# the same way as the main hardening block: skipped under cosmo
# and HULL_DISABLE_HARDENING=1.  FORTIFY adds an extra release-only
# gate so DEBUG builds (ASan/MSan) don't fight with FORTIFY's
# string/mem intercepts.
MINIZ_HARDEN := $(if $(or $(HULL_DISABLE_HARDENING),$(COSMO)),,-fstack-protector-strong -fPIE)
MINIZ_FORTIFY := $(if $(or $(HULL_DISABLE_HARDENING),$(COSMO),$(DEBUG)),,-D_FORTIFY_SOURCE=3)
$(MINIZ_OBJ): $(MINIZ_DIR)/miniz.c | $(BUILDDIR)
	$(CC) -std=c11 -O2 -I$(MINIZ_DIR) -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO \
	    $(MINIZ_HARDEN) $(MINIZ_FORTIFY) $(HARDEN_CFLAGS) \
	    -w $(DEPFLAGS) -c -o $@ $<

# Worker DB (runtime-agnostic per-worker SQLite connections)
$(WORKER_DB_OBJ): $(SRCDIR)/hull/worker_db.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Worker WASM (runtime-agnostic WASM thread pool dispatch)
$(WORKER_WASM_OBJ): $(SRCDIR)/hull/worker_wasm.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(WAMR_CFLAGS) -c -o $@ $<

# Worker GPU (runtime-agnostic GPU thread pool dispatch - base-resident,
# like worker_db/worker_wasm; the wgpu backend itself stays feature/HL_ENABLE_GPU).
$(WORKER_GPU_OBJ): $(SRCDIR)/hull/worker_gpu.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Manifest - split into shared helpers + per-runtime extractors (item G).
# Each per-runtime .c compiles to an empty TU when its runtime is disabled,
# so no special -D filtering is needed for the test-binary single-runtime
# variants - those just pull in the relevant {manifest_lua.o, manifest_js.o}
# alongside manifest.o.
$(BUILDDIR)/manifest.o: $(SRCDIR)/hull/manifest.c $(SRCDIR)/hull/manifest_internal.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/manifest_lua.o: $(SRCDIR)/hull/manifest_lua.c $(SRCDIR)/hull/manifest_internal.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/manifest_js.o: $(SRCDIR)/hull/manifest_js.c $(SRCDIR)/hull/manifest_internal.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# manifest_extract_file.o - runtime-neutral helper that spins up a
# transient HlJS to read app.manifest({...}) from a .js entry point.
# Lives outside the manifest_lua/manifest_js split because it ties the
# JS extractor to a file-on-disk + transient-runtime workflow, not the
# pre-existing "runtime is already running" extractor flow.
$(BUILDDIR)/manifest_extract_file.o: $(SRCDIR)/hull/manifest_extract_file.c $(INCDIR)/hull/manifest_extract_file.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Module registry - canonical sorted table of first-party modules
$(MODULE_REGISTRY_OBJ): $(SRCDIR)/hull/module_registry.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Module resolver - validates manifest.modules into a frozen set
$(MODULE_RESOLVER_OBJ): $(SRCDIR)/hull/module_resolver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# cap/test.c (shared dispatch - no runtime deps, used by both runtimes)
$(BUILDDIR)/cap_test_dispatch.o: $(SRCDIR)/hull/cap/test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Sandbox (pledge/unveil enforcement)
$(SANDBOX_OBJ): $(SRCDIR)/hull/sandbox.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(SANDBOX_TOOL_OBJ): $(SRCDIR)/hull/sandbox_tool.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(EMBED_OBJ): $(SRCDIR)/hull/embed.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Signature verification
$(SIG_OBJ): $(SRCDIR)/hull/signature.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Release artifact signing / verification
$(RELEASE_OBJ): $(SRCDIR)/hull/release.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Shared HTTPS/manifest/atomic-install helpers (hull update + hull tools
# install) plus the always-needed offline release helpers (verify-self +
# platform-sig verifier). The HTTPS half is #ifdef'd inside the TU; the
# object is always built.
$(BUILDDIR)/release_io.o: $(SRCDIR)/hull/release_io.c $(INCDIR)/hull/release_io.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

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
# what actually short-circuits - the symbols just have to compile.
#
# Note the lack of a SOURCE prereq - make never RErefreshes the
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

# Runtime factory registry (table-driven runtime selection - item K)
$(RUNTIME_FACTORY_OBJ): $(SRCDIR)/hull/runtime/factory.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Explicit empty default for the runtime-factory seam (see the var block above).
$(RUNTIME_FACTORY_NONE_OBJ): $(SRCDIR)/hull/runtime/factory_none.c | $(BUILDDIR)
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

# Composable seam for runtime-owned stdlib VFS entries (weak hook + the
# base-union-features platform-VFS init). Runtime-agnostic; base-resident.
$(STDLIB_FEATURE_OBJ): $(SRCDIR)/hull/stdlib_feature.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Shared path-normalize helper (used by both runtimes' module loaders)
$(PATH_NORM_OBJ): $(SRCDIR)/hull/utils/path_normalize.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(THREAD_AFFINITY_OBJ): $(SRCDIR)/hull/shared/thread_affinity.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(LOG_LOCK_OBJ): $(SRCDIR)/hull/shared/log_lock.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# $HOME/.hull/cache/ resolver shared by every runtime cache consumer.
$(CACHE_DIR_OBJ): $(SRCDIR)/hull/shared/cache_dir.c $(INCDIR)/hull/shared/cache_dir.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Shared directory-creation helpers (hl_ensure_dir / hl_mkdir_p).
$(FS_UTIL_OBJ): $(SRCDIR)/hull/shared/fs_util.c $(INCDIR)/hull/shared/fs_util.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Low-level CAS shared by cap/blob.c and the runtime caches.
$(BLOB_STORE_OBJ): $(SRCDIR)/hull/shared/blob_store.c $(INCDIR)/hull/shared/blob_store.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Cache kind registry - used by `hull cache list|prune|clear`.
$(CACHE_REGISTRY_OBJ): $(SRCDIR)/hull/shared/cache_registry.c $(INCDIR)/hull/shared/cache_registry.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Blocking TLS client helper (needs Keel headers; only built when HL_LINK_TLS).
$(BUILDDIR)/tls_client.o: $(SRCDIR)/hull/shared/tls_client.c $(INCDIR)/hull/shared/tls_client.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/tls_transport.o: $(SRCDIR)/hull/shared/tls_transport.c $(INCDIR)/hull/tls_transport.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/tls_transport_stub.o: $(SRCDIR)/hull/tls_transport_stub.c $(INCDIR)/hull/tls_transport.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# App context (shared init for agent, test, MCP)
$(APP_CONTEXT_OBJ): $(SRCDIR)/hull/app_context.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
# Toolchain-only runtime-typed accessors (hl_app_context_lua/js/is_lua);
# force-loaded into hull, kept out of the produced-app runtime path.
$(APP_CONTEXT_RT_OBJ): $(SRCDIR)/hull/app_context_runtime.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Agent library (shared by CLI, MCP, HTTP endpoints) - one .o per
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

# Tool orchestration - cross-layer bindings spliced onto the `tool`
# global after the runtime/lua thin-binding layer installs the base
# table. Lives at src/hull/ (not runtime/lua/) so commands/, dev_state,
# agent_lib, migrate, and module_* aren't pulled into runtime/ headers.
$(BUILDDIR)/tool_orchestration.o: $(SRCDIR)/hull/tool_orchestration.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Build assets (embedded platform lib - stub unless HL_BUILD_EMBEDDED=1).
$(BUILD_ASSET_OBJ): $(SRCDIR)/hull/build_assets.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Build assets stub (no-op stubs for platform archive - satisfies cap_tool.o refs)
$(BUILD_ASSET_STUB_OBJ): $(SRCDIR)/hull/build_assets_stub.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# SBOM (Software Bill of Materials - self-describing vendored-deps table).
# Read-only data exporter. Orthogonal to the rest of the runtime:
# depends only on cacert.h (for embedded-blob SHA-256) and mbedTLS.
$(BUILDDIR)/sbom.o: $(SRCDIR)/hull/sbom.c $(INCDIR)/hull/sbom.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Main (hull_main dispatcher - small; no Keel dependency)
$(BUILDDIR)/main.o: $(SRCDIR)/hull/main.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Serve (full app lifecycle - orchestrates Keel server + runtime)
$(BUILDDIR)/serve.o: $(SRCDIR)/hull/serve.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
# Slim produced-app entry (hl_app_run -> hull_serve); no hull CLI dispatch.
$(APP_RUNNER_OBJ): $(SRCDIR)/hull/app_runner.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Weak no-op defaults for the per-runtime web bindings (issue #114).
$(HTTP_WEAKSTUB_OBJ): $(SRCDIR)/hull/http_weakstub.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(WASM_WEAKSTUB_OBJ): $(SRCDIR)/hull/wasm_weakstub.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(IMAGE_WEAKSTUB_OBJ): $(SRCDIR)/hull/image_weakstub.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Serve-cli (CLI counterpart, used when HL_ENABLE_HTTP_SERVER=0)
$(BUILDDIR)/serve_cli.o: $(SRCDIR)/hull/serve_cli.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Entry (thin main → hull_main trampoline - NOT in platform .a)
$(ENTRY_OBJ): $(SRCDIR)/hull/entry.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# CA bundle accessor (always compiled - empty when HL_EMBED_CA_BUNDLE=0)
ifeq ($(HL_EMBED_CA_BUNDLE),1)
$(CACERT_OBJ): $(SRCDIR)/hull/cacert.c $(INCDIR)/hull/cacert.h $(EMBEDDED_CACERT_H) | $(BUILDDIR)
else
$(CACERT_OBJ): $(SRCDIR)/hull/cacert.c $(INCDIR)/hull/cacert.h | $(BUILDDIR)
endif
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Sealed-arena vendored utility - Hull-local compile so sanitizer
# builds get an instrumented copy.  Source at vendor/sh_seal_arena/
# (Hull's own copy, extracted from Keel v2.7.1; Keel v3 dropped it).
$(SH_SEAL_ARENA_OBJ): $(VENDDIR)/sh_seal_arena/sh_seal_arena.c $(VENDDIR)/sh_seal_arena/sh_seal_arena.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Content-Security-Policy preset registry.
$(CSP_OBJ): $(SRCDIR)/hull/utils/csp.c $(INCDIR)/hull/utils/csp.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Host allowlist matcher (glob + CIDR), shared by db / http / smtp.
$(HOST_MATCH_OBJ): $(SRCDIR)/hull/utils/host_match.c $(INCDIR)/hull/host_match.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(HEX_OBJ): $(SRCDIR)/hull/utils/hex.c $(SRCDIR)/hull/utils/hex.h | $(BUILDDIR)
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
	@echo 'DEBUG := 1' > $(SANITIZER_STAMP)
	@echo "note: objects are ASan-instrumented; a bare 'make test' now inherits DEBUG."

# Unit tests / MSan / fuzzing / e2e live in mk/tests.mk.
include mk/tests.mk
# ── Self-build (hull builds a runnable app binary) ──────────────────
#
# A produced binary is a slim app-runner (hl_app_run -> hull_serve), NOT the
# hull CLI: it runs its own embedded app and has no subcommand dispatcher. So
# the old "hull2 keygen / hull2 builds hull3" chain no longer applies (hull2 is
# just the built app). This target instead proves the build pipeline end to
# end: `hull build` an app.main fixture, then RUN it and require exit 0 - i.e.
# the composed single-runtime binary actually boots (this is what catches an
# "app context init failed" at runtime). Byte-level determinism is covered by
# the separate reproducibility target below.

self-build: $(BUILDDIR)/hull platform $(RUNTIME_FEATURE_LIBS)
	@echo "=== Self-build: hull builds a runnable app binary ==="
	@# --no-verify-platform: this hull is built without EMBED_PLATFORM=1
	@# (the dev/CI default) so it has no embedded signed manifest and
	@# can't satisfy the v0.1.3 platform-sig cross-check. Self-build is
	@# verifying the build pipeline itself, not the trust chain.
	@TMPDIR=$$(mktemp -d) && \
	$(BUILDDIR)/hull build --no-verify-platform -o "$$TMPDIR/app" tests/fixtures/selfbuild_app && \
	"$$TMPDIR/app" && \
	echo "PASS: hull build produced a runnable app-runner (exit 0)" && \
	rm -rf "$$TMPDIR" || \
	(echo "FAIL: self-build" && rm -rf "$$TMPDIR" && exit 1)

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
# source-name.
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
	$(MAKE) $(VEND_OBJS) $(MBEDTLS_OBJS) $(MINIZ_OBJ) $(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS) $(WAMR_OBJS) $(KEEL_LIB)
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
		$(SRCDIR)/hull/main.c $(SRCDIR)/hull/utils/alloc.c $(SRCDIR)/hull/static.c $(SRCDIR)/hull/app_context.c $(SRCDIR)/hull/agent/*.c $(SRCDIR)/hull/agent_api.c $(SRCDIR)/hull/cap/*.c \
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

# Mapped-span performance benchmark (#337 follow-up): host-mmap'd file window read
# by a wasm32 guest via a HullSpan, vs native mmap + copy-once + chunked-copy.
# Two embedded guests: the committed .wasm (interpreter fallback -- lets the wasm
# impls + the correctness gate run WITHOUT wamrc, e.g. locally) and the wamrc-built
# .aot (preferred at runtime; the perf comparand). The AOT is emitted WITH
# --enable-shared-heap (a mapped span IS a shared heap and IS attached for the
# call) via the generic wamrc-AOT-to-header emitter GEN_MEM64_AOT (the guest is
# wasm32, not Memory64, but the emitter only runs wamrc + xxd). Empty AOT fixture
# when wamrc is absent => the bench runs under the interpreter and reports
# engine=interp; the CI job builds wamrc and asserts engine=aot (must-not-skip).
$(BUILDDIR)/gen_bench_span_aot.h: bench/wasm/bench_span_guest.wasm | $(BUILDDIR)
	$(call GEN_MEM64_AOT,$@,bench_span_aot,--enable-shared-heap)

# Committed guest .wasm embedded as a C array (interpreter fallback, always present).
$(BUILDDIR)/gen_bench_span_wasm.h: bench/wasm/bench_span_guest.wasm | $(BUILDDIR)
	@(cd $(dir $<) && xxd -i $(notdir $<)) \
	  | sed -E 's/unsigned char.*\[\]/static const unsigned char bench_span_wasm[]/; s/unsigned int.*_len/static const unsigned int bench_span_wasm_len/' > $@
	@echo "  [bench-span] embedded interpreter-fallback guest .wasm"

# Bench-private cap_wasm object: the ONLY object compiled with the host-call
# counter (production's shared build/cap_wasm.o stays counter-free -- zero cost).
$(BUILDDIR)/bench_cap_wasm.o: $(SRCDIR)/hull/cap/wasm.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -DHL_WASM_HOST_CALL_COUNTER $(INCLUDES) -c -o $@ $<

# Link the bench against the counter-instrumented cap_wasm, not the shared one.
BENCH_SPAN_LIBS := $(filter-out $(BUILDDIR)/cap_wasm.o,$(TEST_COMMON_LIBS)) $(BUILDDIR)/bench_cap_wasm.o
BENCH_SPAN_DEPS := $(filter-out $(BUILDDIR)/cap_wasm.o,$(TEST_COMMON_DEPS)) $(BUILDDIR)/bench_cap_wasm.o

$(BUILDDIR)/bench_mapped_span: bench/wasm/bench_mapped_span.c $(BENCH_SPAN_DEPS) $(BUILDDIR)/gen_bench_span_aot.h $(BUILDDIR)/gen_bench_span_wasm.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -DHL_WASM_HOST_CALL_COUNTER $(INCLUDES) -I$(VENDDIR) -Ibench/wasm -Itemplates -I$(BUILDDIR) -o $@ \
		bench/wasm/bench_mapped_span.c $(BENCH_SPAN_LIBS)

bench-mapped-span: $(BUILDDIR)/bench_mapped_span
	$(BUILDDIR)/bench_mapped_span

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

# Enforce that every maintained compute-SDK header copy (hull_compute.h /
# hull_span.h under examples/ + tests/fixtures/, plus templates/hull_span.h) is
# byte-identical to the canonical embedded source in compute.lua. Reports the
# exact drifting path and fails deterministically. (#331)
check-sdk-headers:
	sh tests/check_sdk_headers.sh

# Deterministic negative test: prove check-sdk-headers fails on injected drift.
check-sdk-headers-selftest:
	sh tests/check_sdk_headers_selftest.sh

# Permanent fixture for WAMR patch 0006's MSan shadow-gap annotation: asserts the
# instrumentation policy the annotation depends on (WAMR uninstrumented under MSan,
# -DHL_MSAN passed, normal builds carry neither HL_MSAN nor __msan_unpoison,
# annotation placement/scope, docs framing) against the ACTUAL compile commands.
check-wamr-msan-annotation:
	sh tests/check_wamr_msan_annotation.sh

# Documentation-integrity gate: catalogued docs, resolving Markdown links,
# inventoried archive, resolving first-party docs/ refs, no resurrected historical
# paths. See tests/check_docs_integrity.sh.
check-docs-integrity:
	sh tests/check_docs_integrity.sh

# Deterministic negative test: prove the docs gate bites on each violation class.
check-docs-integrity-selftest:
	sh tests/check_docs_integrity_selftest.sh

# No-em-dash gate: no U+2014 in living first-party prose (H1/S5). Scope + the
# vendor/archive/fixture/LICENSE exclusions are documented in the script.
check-no-emdash:
	sh tests/check_no_emdash.sh

check-no-emdash-selftest:
	sh tests/check_no_emdash_selftest.sh

# No-new-milestone-narration gate: no development-milestone narration shapes in
# the S4-reviewed code + build surface, with the inventory's exact survivors
# allowlisted (H1/S5). See tests/check_no_milestone_narration.sh.
check-no-milestone-narration:
	sh tests/check_no_milestone_narration.sh

check-no-milestone-narration-selftest:
	sh tests/check_no_milestone_narration_selftest.sh

# Site-consistency gate: the marketing site (site/index.html) advertises the
# current CHANGELOG version (JSON-LD + data-hull-version markers) and keeps its
# Linux/macOS/Windows install tabs + the Windows installer. See the script.
check-site-consistency:
	sh tests/check_site_consistency.sh

check-site-consistency-selftest:
	sh tests/check_site_consistency_selftest.sh

lint: lint-lua lint-js check-sdk-headers check-docs-integrity check-no-emdash check-no-milestone-narration check-site-consistency

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
#   docs-api          - build all three
#   docs-api-c        - Doxygen → build/api/c/
#   docs-api-lua      - LDoc    → build/api/lua/
#   docs-api-js       - JSDoc   → build/api/js/
#   docs-api-check    - fail if any required tool is missing

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
	rm -f fuzz/fuzz_sh_json fuzz/fuzz_path_normalize fuzz/fuzz_mime_sniff fuzz/fuzz_host_match fuzz/fuzz_pgwire fuzz/fuzz_pg_dsn fuzz/fuzz_pg_rewrite fuzz/fuzz_mysqlwire fuzz/fuzz_mysql_dsn
	@$(MAKE) -s -C $(KEEL_DIR) clean 2>/dev/null || true

# ── Header-dependency replay ────────────────────────────────────────
#
# Re-include the .d files emitted by -MMD/-MP (see DEPFLAGS above and
# the comment near the top CFLAGS definition). Each .d file is a make
# fragment listing the user headers a .c included; replaying them turns
# a header touch into the correct narrow set of .o rebuilds.
#
# `-include` (with the dash) silently ignores missing .d files on the
# first build - they appear after the first compile pass.
#
# Uses `find` rather than `wildcard` because WAMR objects live in
# nested directories under $(BUILDDIR)/wamr_core/... and shell globbing
# is the simplest portable way to gather all of them.
DEPS_ALL := $(shell find $(BUILDDIR) -name '*.d' 2>/dev/null)
-include $(DEPS_ALL)


